#pragma once

#include <thread>
#include "AppDef.h"
#include "Types.h"
#include "MIPS.h"
#include "MailBox.h"
#include "PadHandler.h"
#include "iso9660/ISO9660.h"
#include "VirtualMachine.h"
#include "Ee_SubSystem.h"
#include "iop/Iop_SubSystem.h"
#include "iop/IopBios.h"
#include "FrameDump.h"
#include "Profiler.h"

enum PS2VM_MSG
{
	PS2VM_MSG_PAUSE,
	PS2VM_MSG_RESUME,
	PS2VM_MSG_DESTROY,
	PS2VM_MSG_CREATEGS,
	PS2VM_MSG_DESTROYGS,
	PS2VM_MSG_CREATEPAD,
	PS2VM_MSG_DESTROYPAD,
	PS2VM_MSG_SAVESTATE,
	PS2VM_MSG_LOADSTATE,
	PS2VM_MSG_RESET,
};

#define PREF_PS2_HOST_DIRECTORY				("ps2.host.directory")
#define PREF_PS2_MC0_DIRECTORY				("ps2.mc0.directory")
#define PREF_PS2_MC1_DIRECTORY				("ps2.mc1.directory")

class CPS2VM : public CVirtualMachine
{
public:
	typedef std::unique_ptr<Ee::CSubSystem> EeSubSystemPtr;
	typedef std::unique_ptr<Iop::CSubSystem> IopSubSystemPtr;
	typedef std::function<void (const CFrameDump&)> FrameDumpCallback;
	typedef boost::signals2::signal<void (const CProfiler::ZoneArray&)> ProfileFrameDoneSignal;

								CPS2VM();
	virtual						~CPS2VM();

	void						Initialize();
	void						Destroy();

	void						StepEe();
	void						StepIop();
	void						StepVu0();
	void						StepVu1();

	void						Resume();
	void						Pause();
	void						Reset();

	STATUS						GetStatus() const;

	void						DumpEEIntcHandlers();
	void						DumpEEDmacHandlers();

	void						CreateGSHandler(const CGSHandler::FactoryFunction&);
	CGSHandler*					GetGSHandler();
	void						DestroyGSHandler();

	void						CreatePadHandler(const CPadHandler::FactoryFunction&);
	void						DestroyPadHandler();

	unsigned int				SaveState(const char*);
	unsigned int				LoadState(const char*);

	void						TriggerFrameDump(const FrameDumpCallback&);

#ifdef DEBUGGER_INCLUDED
	std::string					MakeDebugTagsPackagePath(const char*);
	void						LoadDebugTags(const char*);
	void						SaveDebugTags(const char*);
#endif

	CPadHandler*				m_pad;

	EeSubSystemPtr				m_ee;
	IopSubSystemPtr				m_iop;

	IopBiosPtr					m_iopOs;

	CISO9660*					m_pCDROM0;

	ProfileFrameDoneSignal		ProfileFrameDone;

private:
	void						CreateVM();
	void						ResetVM();
	void						DestroyVM();
	void						SaveVMState(const char*, unsigned int&);
	void						LoadVMState(const char*, unsigned int&);

	void						ReadToEeRam(uint32, uint32);
	void						ReloadExecutable(const char*, const CPS2OS::ArgumentList&);

	void						ResumeImpl();
	void						PauseImpl();
	void						DestroyImpl();
	void						CreateGsImpl(const CGSHandler::FactoryFunction&);
	void						DestroyGsImpl();

	void						CreatePadHandlerImpl(const CPadHandler::FactoryFunction&);
	void						DestroyPadHandlerImpl();

	void						UpdateEe();
	void						UpdateIop();
	void						UpdateSpu();

	void						OnGsNewFrame();

	void						CDROM0_Initialize();
	void						CDROM0_Mount(const char*);
	void						CDROM0_Reset();
	void						CDROM0_Destroy();
	void						SetIopCdImage(CISO9660*);

	void						RegisterModulesInPadHandler();

	void						EmuThread();

	std::thread					m_thread;
	CMailBox					m_mailBox;
	STATUS						m_nStatus;
	bool						m_nEnd;

	int							m_vblankTicks = 0;
	bool						m_inVblank = 0;
	int							m_spuUpdateTicks = 0;
	int							m_eeExecutionTicks = 0;
	int							m_iopExecutionTicks = 0;

	bool						m_singleStepEe;
	bool						m_singleStepIop;
	bool						m_singleStepVu0;
	bool						m_singleStepVu1;

	CFrameDump					m_frameDump;
	FrameDumpCallback			m_frameDumpCallback;
	std::mutex					m_frameDumpCallbackMutex;
	bool						m_dumpingFrame = false;

	CProfiler::ZoneHandle		m_eeProfilerZone = 0;
	CProfiler::ZoneHandle		m_iopProfilerZone = 0;
	CProfiler::ZoneHandle		m_spuProfilerZone = 0;
	CProfiler::ZoneHandle		m_gsSyncProfilerZone = 0;
	CProfiler::ZoneHandle		m_otherProfilerZone = 0;
};
