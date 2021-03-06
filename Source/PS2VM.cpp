#include <stdio.h>
#include <exception>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <memory>
#include "make_unique.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "PS2OS.h"
#include "Ps2Const.h"
#include "iop/Iop_SifManPs2.h"
#include "VIF.h"
#include "Timer.h"
#include "PtrMacro.h"
#include "StdStream.h"
#include "GZipStream.h"
#ifdef WIN32
#include "VolumeStream.h"
#else
#include "Posix_VolumeStream.h"
#endif
#include "stricmp.h"
#include "IszImageStream.h"
#include "MemoryStateFile.h"
#include "zip/ZipArchiveWriter.h"
#include "zip/ZipArchiveReader.h"
#include "xml/Node.h"
#include "xml/Writer.h"
#include "xml/Parser.h"
#include "AppConfig.h"
#include "PathUtils.h"
#include "iop/IopBios.h"
#include "iop/DirectoryDevice.h"
#include "iop/IsoDevice.h"
#include "Log.h"

#define LOG_NAME		("ps2vm")

#ifdef DEBUGGER_INCLUDED
#define TAGS_PATH		("./tags/")
#endif

#define PREF_PS2_HOST_DIRECTORY_DEFAULT		("vfs/host")
#define PREF_PS2_MC0_DIRECTORY_DEFAULT		("vfs/mc0")
#define PREF_PS2_MC1_DIRECTORY_DEFAULT		("vfs/mc1")

#define FRAME_TICKS			(PS2::EE_CLOCK_FREQ / 60)
#define ONSCREEN_TICKS		(FRAME_TICKS * 9 / 10)
#define VBLANK_TICKS		(FRAME_TICKS / 10)

#define SPU_UPDATE_TICKS	(FRAME_TICKS / 2)

#define VPU_LOG_BASE		"./vpu_logs/"

namespace filesystem = boost::filesystem;

CPS2VM::CPS2VM()
: m_nStatus(PAUSED)
, m_nEnd(false)
, m_pad(NULL)
, m_singleStepEe(false)
, m_singleStepIop(false)
, m_singleStepVu0(false)
, m_singleStepVu1(false)
, m_vblankTicks(0)
, m_inVblank(false)
, m_eeExecutionTicks(0)
, m_iopExecutionTicks(0)
, m_spuUpdateTicks(SPU_UPDATE_TICKS)
, m_pCDROM0(NULL)
, m_eeProfilerZone(CProfiler::GetInstance().RegisterZone("EE"))
, m_iopProfilerZone(CProfiler::GetInstance().RegisterZone("IOP"))
, m_spuProfilerZone(CProfiler::GetInstance().RegisterZone("SPU"))
, m_gsSyncProfilerZone(CProfiler::GetInstance().RegisterZone("GSSYNC"))
, m_otherProfilerZone(CProfiler::GetInstance().RegisterZone("OTHER"))
{
	const char* basicDirectorySettings[] =
	{
		PREF_PS2_HOST_DIRECTORY, PREF_PS2_HOST_DIRECTORY_DEFAULT,
		PREF_PS2_MC0_DIRECTORY, PREF_PS2_MC0_DIRECTORY_DEFAULT,
		PREF_PS2_MC1_DIRECTORY, PREF_PS2_MC1_DIRECTORY_DEFAULT,
		NULL, NULL
	};

	for(unsigned int i = 0; basicDirectorySettings[i] != NULL; i += 2)
	{
		const char* setting = basicDirectorySettings[i + 0];
		const char* path = basicDirectorySettings[i + 1];

		Framework::CConfig::PathType absolutePath = CAppConfig::GetBasePath() / path;
		Framework::PathUtils::EnsurePathExists(absolutePath);
		//TODO: We ought to add a function to write a "path" in the settings. Since it can be wchar_t or char.
		CAppConfig::GetInstance().RegisterPreferenceString(setting, absolutePath.string().c_str());
	}
	
	m_iop = std::make_unique<Iop::CSubSystem>(true);
	m_iopOs = std::make_shared<CIopBios>(m_iop->m_cpu, m_iop->m_ram, PS2::IOP_RAM_SIZE);

	m_ee = std::make_unique<Ee::CSubSystem>(m_iop->m_ram, *m_iopOs);
	m_ee->m_os->OnRequestLoadExecutable.connect(boost::bind(&CPS2VM::ReloadExecutable, this, _1, _2));
}

CPS2VM::~CPS2VM()
{
	{
		//Big hack to force deletion of the IopBios
		m_iop->SetBios(Iop::BiosBasePtr());
		m_iopOs.reset();
	}
}

//////////////////////////////////////////////////
//Various Message Functions
//////////////////////////////////////////////////

void CPS2VM::CreateGSHandler(const CGSHandler::FactoryFunction& factoryFunction)
{
	if(m_ee->m_gs != NULL) return;
	m_mailBox.SendCall(bind(&CPS2VM::CreateGsImpl, this, factoryFunction), true);
}

CGSHandler* CPS2VM::GetGSHandler()
{
	return m_ee->m_gs;
}

void CPS2VM::DestroyGSHandler()
{
	if(m_ee->m_gs == NULL) return;
	m_mailBox.SendCall(std::bind(&CPS2VM::DestroyGsImpl, this), true);
}

void CPS2VM::CreatePadHandler(const CPadHandler::FactoryFunction& factoryFunction)
{
	if(m_pad != NULL) return;
	m_mailBox.SendCall(std::bind(&CPS2VM::CreatePadHandlerImpl, this, factoryFunction), true);
}

void CPS2VM::DestroyPadHandler()
{
	if(m_pad == NULL) return;
	m_mailBox.SendCall(std::bind(&CPS2VM::DestroyPadHandlerImpl, this), true);
}

CVirtualMachine::STATUS CPS2VM::GetStatus() const
{
	return m_nStatus;
}

void CPS2VM::StepEe()
{
	if(GetStatus() == RUNNING) return;
	m_singleStepEe = true;
	m_mailBox.SendCall(std::bind(&CPS2VM::ResumeImpl, this), true);
}

void CPS2VM::StepIop()
{
	if(GetStatus() == RUNNING) return;
	m_singleStepIop = true;
	m_mailBox.SendCall(std::bind(&CPS2VM::ResumeImpl, this), true);
}

void CPS2VM::StepVu0()
{
	if(GetStatus() == RUNNING) return;
	m_singleStepVu0 = true;
	m_mailBox.SendCall(std::bind(&CPS2VM::ResumeImpl, this), true);
}

void CPS2VM::StepVu1()
{
	if(GetStatus() == RUNNING) return;
	m_singleStepVu1 = true;
	m_mailBox.SendCall(std::bind(&CPS2VM::ResumeImpl, this), true);
}

void CPS2VM::Resume()
{
	if(m_nStatus == RUNNING) return;
	m_mailBox.SendCall(std::bind(&CPS2VM::ResumeImpl, this), true);
	OnRunningStateChange();
}

void CPS2VM::Pause()
{
	if(m_nStatus == PAUSED) return;
	m_mailBox.SendCall(std::bind(&CPS2VM::PauseImpl, this), true);
	OnMachineStateChange();
	OnRunningStateChange();
}

void CPS2VM::Reset()
{
	assert(m_nStatus == PAUSED);
	ResetVM();
}

void CPS2VM::DumpEEIntcHandlers()
{
//	if(m_pOS == NULL) return;
	if(m_nStatus != PAUSED) return;
	m_ee->m_os->DumpIntcHandlers();
}

void CPS2VM::DumpEEDmacHandlers()
{
//	if(m_pOS == NULL) return;
	if(m_nStatus != PAUSED) return;
	m_ee->m_os->DumpDmacHandlers();
}

void CPS2VM::Initialize()
{
	CreateVM();
	m_nEnd = false;
	m_thread = std::thread([&] () { EmuThread(); });
}

void CPS2VM::Destroy()
{
	m_mailBox.SendCall(std::bind(&CPS2VM::DestroyImpl, this));
	m_thread.join();
	DestroyVM();
}

unsigned int CPS2VM::SaveState(const char* sPath)
{
	unsigned int result = 0;
	m_mailBox.SendCall(std::bind(&CPS2VM::SaveVMState, this, sPath, std::ref(result)), true);
	return result;
}

unsigned int CPS2VM::LoadState(const char* sPath)
{
	unsigned int result = 0;
	m_mailBox.SendCall(std::bind(&CPS2VM::LoadVMState, this, sPath, std::ref(result)), true);
	return result;
}

void CPS2VM::TriggerFrameDump(const FrameDumpCallback& frameDumpCallback)
{
	m_mailBox.SendCall(
		[=] ()
		{
			std::unique_lock<std::mutex> frameDumpCallbackMutexLock(m_frameDumpCallbackMutex);
			if(m_frameDumpCallback) return;
			m_frameDumpCallback = frameDumpCallback;
		},
		false
	);
}

#ifdef DEBUGGER_INCLUDED

#define TAGS_SECTION_TAGS			("tags")
#define TAGS_SECTION_EE_FUNCTIONS	("ee_functions")
#define TAGS_SECTION_EE_COMMENTS	("ee_comments")
#define TAGS_SECTION_VU1_FUNCTIONS	("vu1_functions")
#define TAGS_SECTION_VU1_COMMENTS	("vu1_comments")
#define TAGS_SECTION_IOP			("iop")
#define TAGS_SECTION_IOP_FUNCTIONS	("functions")
#define TAGS_SECTION_IOP_COMMENTS	("comments")

std::string CPS2VM::MakeDebugTagsPackagePath(const char* packageName)
{
	filesystem::path tagsPath(TAGS_PATH);
	if(!filesystem::exists(tagsPath))
	{
		filesystem::create_directory(tagsPath);
	}
	return std::string(TAGS_PATH) + std::string(packageName) + std::string(".tags.xml");
}

void CPS2VM::LoadDebugTags(const char* packageName)
{
	try
	{
		std::string packagePath = MakeDebugTagsPackagePath(packageName);
		Framework::CStdStream stream(packagePath.c_str(), "rb");
		boost::scoped_ptr<Framework::Xml::CNode> document(Framework::Xml::CParser::ParseDocument(stream));
		Framework::Xml::CNode* tagsNode = document->Select(TAGS_SECTION_TAGS);
		if(!tagsNode) return;
		m_ee->m_EE.m_Functions.Unserialize(tagsNode, TAGS_SECTION_EE_FUNCTIONS);
		m_ee->m_EE.m_Comments.Unserialize(tagsNode, TAGS_SECTION_EE_COMMENTS);
		m_ee->m_VU1.m_Functions.Unserialize(tagsNode, TAGS_SECTION_VU1_FUNCTIONS);
		m_ee->m_VU1.m_Comments.Unserialize(tagsNode, TAGS_SECTION_VU1_COMMENTS);
		{
			Framework::Xml::CNode* sectionNode = tagsNode->Select(TAGS_SECTION_IOP);
			if(sectionNode)
			{
				m_iop->m_cpu.m_Functions.Unserialize(sectionNode, TAGS_SECTION_IOP_FUNCTIONS);
				m_iop->m_cpu.m_Comments.Unserialize(sectionNode, TAGS_SECTION_IOP_COMMENTS);
				m_iopOs->LoadDebugTags(sectionNode);
			}
		}
	}
	catch(...)
	{

	}
}

void CPS2VM::SaveDebugTags(const char* packageName)
{
	try
	{
		std::string packagePath = MakeDebugTagsPackagePath(packageName);
		Framework::CStdStream stream(packagePath.c_str(), "wb");
		boost::scoped_ptr<Framework::Xml::CNode> document(new Framework::Xml::CNode(TAGS_SECTION_TAGS, true)); 
		m_ee->m_EE.m_Functions.Serialize(document.get(), TAGS_SECTION_EE_FUNCTIONS);
		m_ee->m_EE.m_Comments.Serialize(document.get(), TAGS_SECTION_EE_COMMENTS);
		m_ee->m_VU1.m_Functions.Serialize(document.get(), TAGS_SECTION_VU1_FUNCTIONS);
		m_ee->m_VU1.m_Comments.Serialize(document.get(), TAGS_SECTION_VU1_COMMENTS);
		{
			Framework::Xml::CNode* iopNode = new Framework::Xml::CNode(TAGS_SECTION_IOP, true);
			m_iop->m_cpu.m_Functions.Serialize(iopNode, TAGS_SECTION_IOP_FUNCTIONS);
			m_iop->m_cpu.m_Comments.Serialize(iopNode, TAGS_SECTION_IOP_COMMENTS);
			m_iopOs->SaveDebugTags(iopNode);
			document->InsertNode(iopNode);
		}
		Framework::Xml::CWriter::WriteDocument(stream, document.get());
	}
	catch(...)
	{

	}
}

#endif

//////////////////////////////////////////////////
//Non extern callable methods
//////////////////////////////////////////////////

void CPS2VM::CreateVM()
{
	CDROM0_Initialize();
	ResetVM();
}

void CPS2VM::ResetVM()
{
	m_ee->Reset();

	m_iop->Reset();
	m_iop->SetBios(m_iopOs);

	//LoadBIOS();

	if(m_ee->m_gs != NULL)
	{
		m_ee->m_gs->Reset();
	}

	m_iopOs->Reset(new Iop::CSifManPs2(m_ee->m_sif, m_ee->m_ram, m_iop->m_ram));

	CDROM0_Reset();

	m_iopOs->GetIoman()->RegisterDevice("host", Iop::CIoman::DevicePtr(new Iop::Ioman::CDirectoryDevice(PREF_PS2_HOST_DIRECTORY)));
	m_iopOs->GetIoman()->RegisterDevice("mc0", Iop::CIoman::DevicePtr(new Iop::Ioman::CDirectoryDevice(PREF_PS2_MC0_DIRECTORY)));
	m_iopOs->GetIoman()->RegisterDevice("mc1", Iop::CIoman::DevicePtr(new Iop::Ioman::CDirectoryDevice(PREF_PS2_MC1_DIRECTORY)));
	m_iopOs->GetIoman()->RegisterDevice("cdrom0", Iop::CIoman::DevicePtr(new Iop::Ioman::CIsoDevice(m_pCDROM0)));

	m_iopOs->GetLoadcore()->SetLoadExecutableHandler(std::bind(&CPS2OS::LoadExecutable, m_ee->m_os, std::placeholders::_1, std::placeholders::_2));

	m_iopOs->GetCdvdfsv()->SetReadToEeRamHandler(std::bind(&CPS2VM::ReadToEeRam, this, std::placeholders::_1, std::placeholders::_2));

	m_vblankTicks = ONSCREEN_TICKS;
	m_inVblank = false;

	m_eeExecutionTicks = 0;
	m_iopExecutionTicks = 0;

	RegisterModulesInPadHandler();

#ifdef DEBUGGER_INCLUDED
	try
	{
		boost::filesystem::path logPath(VPU_LOG_BASE);
		boost::filesystem::remove_all(logPath);
		boost::filesystem::create_directory(logPath);
	}
	catch(...)
	{

	}
#endif
}

void CPS2VM::DestroyVM()
{
	CDROM0_Destroy();
}

void CPS2VM::SaveVMState(const char* sPath, unsigned int& result)
{
	if(m_ee->m_gs == NULL)
	{
		printf("PS2VM: GS Handler was not instancied. Cannot save state.\r\n");
		result = 1;
		return;
	}

	try
	{
		Framework::CStdStream stateStream(sPath, "wb");
		Framework::CZipArchiveWriter archive;

		m_ee->SaveState(archive);
		m_iop->SaveState(archive);
		m_ee->m_gs->SaveState(archive);
		m_iopOs->GetPadman()->SaveState(archive);
		//TODO: Save CDVDFSV state

		archive.Write(stateStream);
	}
	catch(...)
	{
		result = 1;
		return;
	}

	printf("PS2VM: Saved state to file '%s'.\r\n", sPath);

	result = 0;
}

void CPS2VM::LoadVMState(const char* sPath, unsigned int& result)
{
	if(m_ee->m_gs == NULL)
	{
		printf("PS2VM: GS Handler was not instancied. Cannot load state.\r\n");
		result = 1;
		return;
	}

	try
	{
		Framework::CStdStream stateStream(sPath, "rb");
		Framework::CZipArchiveReader archive(stateStream);
		
		try
		{
			m_ee->LoadState(archive);
			m_iop->LoadState(archive);
			m_ee->m_gs->LoadState(archive);
			m_iopOs->GetPadman()->LoadState(archive);
		}
		catch(...)
		{
			//Any error that occurs in the previous block is critical
			PauseImpl();
			throw;
		}
	}
	catch(...)
	{
		result = 1;
		return;
	}

	printf("PS2VM: Loaded state from file '%s'.\r\n", sPath);

	OnMachineStateChange();

	result = 0;
}

void CPS2VM::PauseImpl()
{
	m_nStatus = PAUSED;
}

void CPS2VM::ResumeImpl()
{
#ifdef DEBUGGER_INCLUDED
	m_ee->m_executor.DisableBreakpointsOnce();
	m_iop->m_executor.DisableBreakpointsOnce();
	m_ee->m_vif.DisableVu1BreakpointsOnce();
#endif
	m_nStatus = RUNNING;
}

void CPS2VM::DestroyImpl()
{
	DELETEPTR(m_ee->m_gs);
	m_nEnd = true;
}

void CPS2VM::CreateGsImpl(const CGSHandler::FactoryFunction& factoryFunction)
{
	m_ee->m_gs = factoryFunction();
	m_ee->m_gs->Initialize();
	m_ee->m_gs->OnNewFrame.connect(boost::bind(&CPS2VM::OnGsNewFrame, this));
}

void CPS2VM::DestroyGsImpl()
{
	m_ee->m_gs->Release();
	DELETEPTR(m_ee->m_gs);
}

void CPS2VM::CreatePadHandlerImpl(const CPadHandler::FactoryFunction& factoryFunction)
{
	m_pad = factoryFunction();
	RegisterModulesInPadHandler();
}

void CPS2VM::DestroyPadHandlerImpl()
{
	DELETEPTR(m_pad);
}

void CPS2VM::OnGsNewFrame()
{
#ifdef DEBUGGER_INCLUDED
	std::unique_lock<std::mutex> dumpFrameCallbackMutexLock(m_frameDumpCallbackMutex);
	if(m_dumpingFrame)
	{
		m_ee->m_gs->SetFrameDump(nullptr);
		m_frameDumpCallback(m_frameDump);
		m_dumpingFrame = false;
		m_frameDumpCallback = FrameDumpCallback();
	}
	else if(m_frameDumpCallback)
	{
		m_frameDump.Reset();
		memcpy(m_frameDump.GetInitialGsRam(), m_ee->m_gs->GetRam(), CGSHandler::RAMSIZE);
		memcpy(m_frameDump.GetInitialGsRegisters(), m_ee->m_gs->GetRegisters(), CGSHandler::REGISTER_MAX * sizeof(uint64));
		m_frameDump.SetInitialSMODE2(m_ee->m_gs->GetSMODE2());
		m_ee->m_gs->SetFrameDump(&m_frameDump);
		m_dumpingFrame = true;
	}
#endif
}

void CPS2VM::UpdateEe()
{
#ifdef PROFILE
	CProfilerZone profilerZone(m_eeProfilerZone);
#endif

	while(m_eeExecutionTicks > 0)
	{
		int executed = m_ee->ExecuteCpu(m_singleStepEe ? 1 : m_eeExecutionTicks);
		if(m_ee->IsCpuIdle())
		{
			executed = m_eeExecutionTicks;
		}

		m_eeExecutionTicks -= executed;
		m_ee->CountTicks(executed);
		m_vblankTicks -= executed;
		m_spuUpdateTicks -= executed;

		//Stop executing if executing VU subroutine
		if(m_ee->m_EE.m_State.callMsEnabled) break;

#ifdef DEBUGGER_INCLUDED
		if(m_singleStepEe) break;
		if(m_ee->m_executor.MustBreak()) break;
#endif
	}
}

void CPS2VM::UpdateIop()
{
#ifdef PROFILE
	CProfilerZone profilerZone(m_iopProfilerZone);
#endif

	while(m_iopExecutionTicks > 0)
	{
		int executed = m_iop->ExecuteCpu(m_singleStepIop ? 1 : m_iopExecutionTicks);
		if(m_iop->IsCpuIdle())
		{
			executed = m_iopExecutionTicks;
		}

		m_iopExecutionTicks -= executed;
		m_iop->CountTicks(executed);

#ifdef DEBUGGER_INCLUDED
		if(m_singleStepIop) break;
		if(m_iop->m_executor.MustBreak()) break;
#endif
	}
}

void CPS2VM::UpdateSpu()
{
#ifdef PROFILE
	CProfilerZone profilerZone(m_spuProfilerZone);
#endif

	Iop::CSpuBase* spu[2] = { &m_iop->m_spuCore0, &m_iop->m_spuCore1 };
	const int sampleRate = 44100;
	const int sampleCount = 352;
	size_t bufferSize = sampleCount * sizeof(int16);

	for(unsigned int i = 0; i < 2; i++)
	{
		if(spu[i]->IsEnabled())
		{
			int16* tempSamples = reinterpret_cast<int16*>(alloca(bufferSize));
			spu[i]->Render(tempSamples, sampleCount, sampleRate);

			//for(unsigned int j = 0; j < sampleCount; j++)
			//{
			//	int32 resultSample = static_cast<int32>(samples[j]) + static_cast<int32>(tempSamples[j]);
			//	resultSample = max<int32>(resultSample, SHRT_MIN);
			//	resultSample = min<int32>(resultSample, SHRT_MAX);
			//	samples[j] = static_cast<int16>(resultSample);
			//}
		}
	}
}

void CPS2VM::CDROM0_Initialize()
{
	CAppConfig::GetInstance().RegisterPreferenceString(PS2VM_CDROM0PATH, "");
	m_pCDROM0 = NULL;
}

void CPS2VM::CDROM0_Reset()
{
	DELETEPTR(m_pCDROM0);
	CDROM0_Mount(CAppConfig::GetInstance().GetPreferenceString(PS2VM_CDROM0PATH));
}

void CPS2VM::CDROM0_Mount(const char* sPath)
{
	//Check if there's an m_pCDROM0 already
	//Check if files are linked to this m_pCDROM0 too and do something with them

	size_t pathLength = strlen(sPath);
	if(pathLength != 0)
	{
		try
		{
			Framework::CStream* pStream(NULL);
			const char* extension = "";
			if(pathLength >= 4)
			{
				extension = sPath + pathLength - 4;
			}

			//Gotta think of something better than that...
#ifndef __ANDROID__		//This is temporary till we get bzip2 to compile on Android
			if(!stricmp(extension, ".isz"))
			{
				pStream = new CIszImageStream(new Framework::CStdStream(sPath, "rb"));
			}
#endif
#ifdef WIN32
			else if(sPath[0] == '\\')
			{
				pStream = new Framework::Win32::CVolumeStream(sPath[4]);
			}
#elif !defined(__ANDROID__)
			else
			{
				pStream = new Framework::Posix::CVolumeStream(sPath);
			}
#endif

			//If it's null after all that, just feed it to a StdStream
			if(pStream == NULL)
			{
				pStream = new Framework::CStdStream(sPath, "rb");
			}

			m_pCDROM0 = new CISO9660(pStream);
			SetIopCdImage(m_pCDROM0);
		}
		catch(const std::exception& Exception)
		{
			printf("PS2VM: Error mounting cdrom0 device: %s\r\n", Exception.what());
		}
	}

	CAppConfig::GetInstance().SetPreferenceString(PS2VM_CDROM0PATH, sPath);
}

void CPS2VM::CDROM0_Destroy()
{
	SetIopCdImage(NULL);
	DELETEPTR(m_pCDROM0);
}

void CPS2VM::SetIopCdImage(CISO9660* image)
{
	m_iopOs->GetCdvdfsv()->SetIsoImage(image);
	m_iopOs->GetCdvdman()->SetIsoImage(image);
}

void CPS2VM::RegisterModulesInPadHandler()
{
	if(m_pad == NULL) return;

	m_pad->RemoveAllListeners();
	m_pad->InsertListener(m_iopOs->GetPadman());
	m_pad->InsertListener(&m_iop->m_sio2);
}

void CPS2VM::ReadToEeRam(uint32 address, uint32 size)
{
	m_ee->m_executor.ClearActiveBlocksInRange(address, address + size);
}

void CPS2VM::ReloadExecutable(const char* executablePath, const CPS2OS::ArgumentList& arguments)
{
	ResetVM();
	m_ee->m_os->BootFromCDROM(arguments);
}

void CPS2VM::EmuThread()
{
	CProfiler::GetInstance().SetWorkThread();
	while(1)
	{
		while(m_mailBox.IsPending())
		{
			m_mailBox.ReceiveCall();
		}
		if(m_nEnd) break;
		if(m_nStatus == PAUSED)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if(m_nStatus == RUNNING)
		{
#ifdef PROFILE
			CProfilerZone profilerZone(m_otherProfilerZone);
#endif

			if(m_spuUpdateTicks <= 0)
			{
				UpdateSpu();
				m_spuUpdateTicks += SPU_UPDATE_TICKS;
			}

			//EE execution
			{
				//Check vblank stuff
				if(m_vblankTicks <= 0)
				{
					m_inVblank = !m_inVblank;
					if(m_inVblank)
					{
						m_vblankTicks += VBLANK_TICKS;
						m_ee->NotifyVBlankStart();
						m_iop->NotifyVBlankStart();

						if(m_ee->m_gs != NULL)
						{
#ifdef PROFILE
							CProfilerZone profilerZone(m_gsSyncProfilerZone);
#endif
							m_ee->m_gs->SetVBlank();
						}

						if(m_pad != NULL)
						{
							m_pad->Update(m_ee->m_ram);
						}
#ifdef PROFILE
						{
							auto stats = CProfiler::GetInstance().GetStats();
							ProfileFrameDone(stats);
							CProfiler::GetInstance().Reset();
						}
#endif
					}
					else
					{
						m_vblankTicks += ONSCREEN_TICKS;
						m_ee->NotifyVBlankEnd();
						m_iop->NotifyVBlankEnd();
						if(m_ee->m_gs != NULL)
						{
							m_ee->m_gs->ResetVBlank();
						}
					}
				}

				//EE CPU is 8 times faster than the IOP CPU
				static const int tickStep = 480;
				m_eeExecutionTicks += tickStep;
				m_iopExecutionTicks += tickStep / 8;

				UpdateEe();
				UpdateIop();

				m_ee->m_vif.ExecuteVu0(m_singleStepVu0);
				m_ee->m_vif.ExecuteVu1(m_singleStepVu1);
			}
#ifdef DEBUGGER_INCLUDED
			if(
			   m_ee->m_executor.MustBreak() || 
			   m_iop->m_executor.MustBreak() ||
			   m_ee->m_vif.MustVu1Break() ||
			   m_singleStepEe || m_singleStepIop || m_singleStepVu0 || m_singleStepVu1)
			{
				m_nStatus = PAUSED;
				m_singleStepEe = false;
				m_singleStepIop = false;
				m_singleStepVu0 = false;
				m_singleStepVu1 = false;
				OnRunningStateChange();
				OnMachineStateChange();
			}
#endif
		}
	}
}
