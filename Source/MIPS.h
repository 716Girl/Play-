#ifndef _MIPS_H_
#define _MIPS_H_

#include "Types.h"
#include "MemoryMap.h"
#include "ExecMap.h"
#include "MIPSArchitecture.h"
#include "MIPSCoprocessor.h"
#include "MIPSAnalysis.h"
#include "MIPSTags.h"
#include "uint128.h"

struct MIPSSTATE
{
	uint32		nPC;
	uint32		nDelayedJumpAddr;

#ifdef WIN32
__declspec(align(16))
#endif
	uint128		nGPR[32];

	uint32		nHI[2];
	uint32		nLO[2];
	uint32		nHI1[2];
	uint32		nLO1[2];
	uint32		nSA;

	//COP0
	uint32		nCOP0[32];

	//COP1
	uint32		nCOP10[32];
	uint32		nCOP11[32];
	uint32		nCOP1A;
	uint32		nFCSR;

	//COP2
#ifdef WIN32
__declspec(align(16))
#endif
	uint128		nCOP2[32];

	uint128		nCOP2A;

	uint128		nCOP2ZF;
	uint128		nCOP2SF;

	uint32		nCOP2Q;
	uint32		nCOP2I;
	uint32		nCOP2P;
	uint32		nCOP2R;
	uint32		nCOP2CF;

	uint32		nCOP2VI[16];
};

#define MIPS_INVALID_PC			(0x00000001)

typedef unsigned int			(*TickFunction)(unsigned int);
typedef void					(*SysCallHandler)();
typedef uint32					(*AddressTranslator)(CMIPS*, uint32, uint32);

class CMIPS
{
public:
								CMIPS(MEMORYMAP_ENDIANESS, uint32, uint32);
								~CMIPS();
	void						ToggleBreakpoint(uint32);
	void						InvalidateCache();
	void						Step();
	RET_CODE					Execute(int);
	bool						MustBreak();
	bool						IsBranch(uint32);
	static long					GetBranch(uint16);
	static uint32				TranslateAddress64(CMIPS*, uint32, uint32);
	static void					DefaultSysCallHandler();

	bool						GenerateInterrupt(uint32);
	bool						GenerateException(uint32);

	MIPSSTATE					m_State;
	uint32						m_nIllOpcode;

	int							m_nQuota;
	CMIPSArchitecture*			m_pArch;
	CMIPSCoprocessor*			m_pCOP[4];
	CMemoryMap*					m_pMemoryMap;
	CExecMap*					m_pExecMap;
	Framework::CList<void>		m_Breakpoint;

	CMIPSAnalysis*				m_pAnalysis;
	CMIPSTags					m_Comments;
	CMIPSTags					m_Functions;

	AddressTranslator			m_pAddrTranslator;
	SysCallHandler				m_pSysCallHandler;
	TickFunction				m_pTickFunction;

	enum REGISTER
	{
		R0 = 0,	AT,	V0,	V1,	A0,	A1,	A2,	A3,
		T0,		T1,	T2,	T3,	T4,	T5,	T6,	T7,
		S0,		S1,	S2,	S3,	S4,	S5,	S6,	S7,
		T8,		T9,	K0,	K1,	GP,	SP,	FP,	RA
	};

	static const char*			m_sGPRName[];
};

#endif
