//
// author: Lasha Khasaia
// contact: @_qaz_qaz
// license: MIT License
//

#include "stdafx.h"

using nlohmann::json;


#define NO_MSG_BOX

ZydisDecoder decoder;

HMODULE copyNtdll{};
HMODULE copyKernelBase{};
BOOL ntCreateDbgObjectCalled = FALSE;
BOOL isDbgCheck = FALSE; // get just one message about IsDebuggerPresent usage
DWORD_PTR memWatchAddress{};
BOOL memWatch = FALSE;
TCHAR tmp[MAX_PATH + 2]{};
TCHAR sys[MAX_PATH + 2]{};

json jsObject{};

VOID HookFunction(const CHAR func[], const DWORD_PTR hookFunc, const TCHAR* libModule)
{
	HMODULE lib;
	if (CompareStringOrdinal(libModule, -1, L"ntdll", -1, FALSE) == CSTR_EQUAL)
	{
		lib = LoadLibrary(L"ntdll");
	}
	else
	{
		lib = LoadLibrary(L"kernelbase");
	}
	if (!lib)
	{
		return;
	}
	auto targetFuncAddress = static_cast<LPVOID>(GetProcAddress(lib, func));

	const ZyanUSize length = 0x10; // MAX INSTR size
	ZydisDecodedInstruction instruction;
	if (!		ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, targetFuncAddress, length,
		&instruction)))
		return;

	const auto nextInstruction = reinterpret_cast<DWORD_PTR>(targetFuncAddress) + instruction.length;

#if defined(_WIN64)
	byte jmp[] = {0x48, 0xB8, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF, 0xE0};
	DWORD old;
	VirtualProtectEx(GetCurrentProcess(), reinterpret_cast<LPVOID>(nextInstruction), 100, PAGE_EXECUTE_READWRITE, &old);
	memcpy_s(reinterpret_cast<PVOID>(nextInstruction), 2, jmp, 2);
	*reinterpret_cast<DWORD_PTR*>(reinterpret_cast<byte*>(nextInstruction) + 2) = static_cast<DWORD_PTR>(hookFunc);
	memcpy_s(reinterpret_cast<byte*>(nextInstruction) + 10, 2, jmp + 10, 2);
	VirtualProtectEx(GetCurrentProcess(), reinterpret_cast<LPVOID>(nextInstruction), 100, old, &old);
#else
	byte jmp[] = { 0x68, 0xCC, 0xCC, 0xCC, 0xCC, 0xC3 };
	DWORD old;
	VirtualProtectEx(GetCurrentProcess(), reinterpret_cast<LPVOID>(nextInstruction), 100, PAGE_EXECUTE_READWRITE, &old);
	memcpy_s(reinterpret_cast<PVOID>(nextInstruction), 1, jmp, 1);
	*reinterpret_cast<DWORD32*>(reinterpret_cast<byte*>(nextInstruction) + 1) = static_cast<DWORD32>(hookFunc);
	memcpy_s(reinterpret_cast<byte*>(nextInstruction) + 5, 1, jmp + 5, 1);
	VirtualProtectEx(GetCurrentProcess(), reinterpret_cast<LPVOID>(nextInstruction), 100, old, &old);
#endif
}


VOID GenRandStr(TCHAR* str, const size_t size) // just enough randomness
{
	srand(static_cast<unsigned int>(time(nullptr)));
	static const TCHAR syms[] =
		L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		L"abcdefghijklmnopqrstuvwxyz";
	for (size_t i = 0; i < size - 1; ++i)
	{
		str[i] = syms[rand() / (RAND_MAX / (_tcslen(syms) - 1) + 1)];
	}
	str[size - 1] = 0;
}

TCHAR* NormalizeRegPath(const LPCTSTR regPath)
{
	const auto pathSize = _tcsclen(regPath);
	auto normalPath = new TCHAR[pathSize + 1]{};
	for (size_t i = 0; i < pathSize; i++)
	{
		if (regPath[i] == *(L"/"))
			normalPath[i] = *(L"\\");
		else
			normalPath[i] = regPath[i];
	}
	return normalPath;
}

// hook functions
LONG WINAPI HookNtClose(_In_ HANDLE handle)
{
	typedef LONG WINAPI NtClose(
		_In_ HANDLE _Handle
	);

	// https://web.archive.org/web/20171214081815/http://winapi.freetechsecrets.com/win32/WIN32GetHandleInformation.htm

	DWORD flags{};
	const auto ret = GetHandleInformation(handle, &flags);

	if (!ret) // invalid handle
	{
		TCHAR msg[0x100]{};
#ifdef _WIN64
		swprintf_s(
			msg,
			L"[NtClose] Invalid HANDLE specified by the debuggee - 0x%llx\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.ii\n",
			DWORD_PTR(handle));
#else
		swprintf_s(msg, L"[NtClose] Invalid HANDLE specified by the debuggee - 0x%x\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.ii\n", DWORD32(handle));
#endif
		OutputDebugString(msg);
		return STATUS_INVALID_HANDLE; // return success
	}
	else if (flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) {
		TCHAR msg[0x100]{};
#ifdef _WIN64
		swprintf_s(
			msg,
			L"[NtClose] The debuggee tries to close PROTECTED HANDLE - 0x%llx\n", DWORD_PTR(handle));
#else
		swprintf_s(msg, L"[NtClose] The debuggee tries to close PROTECTED HANDLE - 0x%x\n", DWORD_PTR(handle));
#endif
		OutputDebugString(msg);
		return 0; // raise exception ??
	}


	const auto close = reinterpret_cast<NtClose*>(GetProcAddress(copyNtdll, "NtClose"));
	return close(handle);
}


LONG WINAPI HookNtOpenProcess(_Out_ PHANDLE ProcessHandle, _In_ ACCESS_MASK DesiredAccess, _In_ LPVOID ObjectAttributes,
                              _In_opt_ PCLIENT_ID ClientId)
{
	typedef LONG WINAPI xZwOpenProcess(
		_Out_ PHANDLE xProcessHandle,
		_In_ ACCESS_MASK xDesiredAccess,
		_In_ LPVOID xObjectAttributes,
		_In_opt_ LPVOID xClientId
	);

	const auto xOP = reinterpret_cast<xZwOpenProcess*>(GetProcAddress(copyNtdll, "NtOpenProcess"));

	TCHAR processName[0x100]{};
	// ProcID
	if (!ClientId)
		return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
	const auto procID = DWORD_PTR(ClientId->UniqueProcess);
	const auto hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);

	PROCESSENTRY32 pe = {sizeof(pe)};
	if (Process32First(hSnapshot, &pe))
	{
		do
		{
			if (pe.th32ProcessID == procID)
			{
				_tcscpy_s(processName, 0x100, pe.szExeFile);
				break;
			}
		}
		while (Process32Next(hSnapshot, &pe));
	}
	CloseHandle(hSnapshot);

	
	auto files = jsObject["Processes"];

	for (auto& file : files)
	{
		auto str_file = file.get<std::string>();
		std::wstring wstr_file(str_file.begin(), str_file.end());

		if (StrStrI(processName, wstr_file.c_str()))
		{
			auto outStr = new TCHAR[0x100]{};
			wsprintf(outStr,
			         L"[NtOpenProcess] [Suspicious behavior] The debuggee attempts to open the following process: %s\n\tref: https://github.com/LordNoteworthy/al-khaser \n",
			         processName);
			OutputDebugString(outStr);

			delete[] outStr;
		}
	}

	return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

LONG WINAPI HookNtCreateFile(
	_Out_ PHANDLE FileHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ POBJECT_ATTRIBUTES ObjectAttributes,
	_Out_ LPVOID IoStatusBlock,
	_In_opt_ PLARGE_INTEGER AllocationSize,
	_In_ ULONG FileAttributes,
	_In_ ULONG ShareAccess,
	_In_ ULONG CreateDisposition,
	_In_ ULONG CreateOptions,
	_In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
	_In_ ULONG EaLength
)
{
	typedef LONG WINAPI xZwCreateFile(
		_Out_ PHANDLE xFileHandle,
		_In_ ACCESS_MASK xDesiredAccess,
		_In_ POBJECT_ATTRIBUTES xObjectAttributes,
		_Out_ LPVOID xIoStatusBlock,
		_In_opt_ PLARGE_INTEGER xAllocationSize,
		_In_ ULONG xFileAttributes,
		_In_ ULONG xShareAccess,
		_In_ ULONG xCreateDisposition,
		_In_ ULONG xCreateOptions,
		_In_reads_bytes_opt_(xEaLength) PVOID
		xEaBuffer,
		_In_ ULONG xEaLength
	);

	// is there a better way to remove \??\ ?
	const auto size = ObjectAttributes->ObjectName->MaximumLength - (4 * sizeof(TCHAR));
	const auto fileName = new TCHAR[size]{};
	_tcscpy_s(fileName, size, ObjectAttributes->ObjectName->Buffer + 4);
	TCHAR cName[MAX_PATH + 2]{};
	DWORD retSize = MAX_PATH + 2;
	QueryFullProcessImageName(GetCurrentProcess(), 0, cName, &retSize);
	// GetModuleFileNameEx(GetCurrentProcess(), nullptr, cName, MAX_PATH + 2);

	if (!_tcscmp(fileName, cName))
	{
		OutputDebugString(
			L"[NtCreateFile] The debuggee attempts to open itself exclusively\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.iii\n");
	}

	const auto devName = new TCHAR[ObjectAttributes->ObjectName->MaximumLength]{};
	_tcscpy_s(devName, ObjectAttributes->ObjectName->MaximumLength, ObjectAttributes->ObjectName->Buffer);

	// The most commonly used I/O devices are as follows: 
	// file, file stream, directory, physical disk, volume, console buffer, tape drive, communications resource, mailslot, and pipe.


	auto files = jsObject["FileSystem"]["Files"];

	for (auto& file : files)
	{
		auto strFile = file.get<std::string>();
		std::wstring wstrFile(strFile.begin(), strFile.end());
		if (wstrFile.find(std::wstring(devName)) != std::wstring::npos) // TODO: case sens.
		{
			auto outStr = new TCHAR[0x100]{};
			wsprintf(outStr,
			         L"[NtCreateFile] The debuggee attempts to open the following suspicious file/directory or I/O device: %s\n\tref: https://github.com/LordNoteworthy/al-khaser \n",
			         devName);
			OutputDebugString(outStr);

			delete[] outStr;
		}
	}

	// Some debuggers open handle when debuggee loads dll and forget to close handle, we can use this behavior to detect a debugger (not all, but IDA Pro at least)
	// Anti-debugger trick: load a dll via LoadLibrary and try to open the same file for opened for exclusive access.
	TCHAR filePath[0x1000]{};
	_tcscpy_s(filePath, 0x1000, L"[_]");
	_tcscpy_s(filePath + 3, 0x1000 - 3, fileName);

	TCHAR fulltmp[MAX_PATH + 2]{};
	GetFullPathName(tmp, MAX_PATH + 2, fulltmp, nullptr);

	if (_tcscmp(fileName, fulltmp) && _tcscmp(fileName, sys))
		OutputDebugString(filePath);

	delete[] fileName;
	delete[] devName;
	const auto oCf = reinterpret_cast<xZwCreateFile*>(GetProcAddress(copyNtdll, "NtCreateFile"));
	return oCf(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess,
	           CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

ULONG WINAPI HookNtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State)
{
	UNREFERENCED_PARAMETER(ComponentId);
	UNREFERENCED_PARAMETER(Level);
	UNREFERENCED_PARAMETER(State);
	// typedef ULONG WINAPI NtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State);
	// auto xNtSetDebugFilterState = reinterpret_cast<NtSetDebugFilterState*>(GetProcAddress(copyNtdll, "NtSetDebugFilterState"));

	OutputDebugString(
		L"[NtSetDebugFilterState] The debuggee attempts to use NtSetDebugFilterState trick\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.vi\n");

	// return xNtSetDebugFilterState(ComponentId, Level, State);
	return 1; // fake it ;)
}

LONG WINAPI HookNtQueryInformationProcess(
	_In_ HANDLE ProcessHandle,
	_In_ PROCESSINFOCLASS ProcessInformationClass,
	_Out_ PVOID ProcessInformation,
	_In_ ULONG ProcessInformationLength,
	_Out_opt_ PULONG ReturnLength
)
{
	typedef LONG WINAPI NtQueryInformationProcess(
		_In_ HANDLE xProcessHandle,
		_In_ PROCESSINFOCLASS
		xProcessInformationClass,
		_Out_ PVOID xProcessInformation,
		_In_ ULONG xProcessInformationLength,
		_Out_opt_ PULONG xReturnLength
	);

	const auto oNQi = reinterpret_cast<NtQueryInformationProcess*>(GetProcAddress(
			copyNtdll, "NtQueryInformationProcess")
	);

	const auto status = oNQi(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength,
	                         ReturnLength);
	if (ProcessInformationClass == ProcessDebugPort)
	{
		OutputDebugString(
			L"[ProcessDebugPort] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.a\n");
		*static_cast<DWORD_PTR*>(ProcessInformation) = 0; // fake it ;)
	}
	if (ProcessInformationClass == ProcessDebugObjectHandle)
	{
		OutputDebugString(
			L"[ProcessDebugObjectHandle] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.b\n");
		*static_cast<DWORD_PTR*>(ProcessInformation) = 0; // fake it ;)
	}

	if (ProcessInformationClass == ProcessDebugFlags)
	{
		OutputDebugString(
			L"[ProcessDebugFlags] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.c\n");
		*static_cast<DWORD*>(ProcessInformation) = 1; // fake it ;)
	}

	return status;
}

LONG WINAPI HookNtQuerySystemInformation(
	ULONG SystemInformationClass,
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength)
{
	typedef LONG WINAPI pNtQuerySystemInformation(
		ULONG xSystemInformationClass,
		PVOID xSystemInformation,
		ULONG xSystemInformationLength,
		PULONG xReturnLength);

	const auto querySysInfo = reinterpret_cast<pNtQuerySystemInformation*>(GetProcAddress(
		copyNtdll, "NtQuerySystemInformation"));
	const ULONG systemKernelDebuggerInformation = 0x23;

	const auto status = querySysInfo(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);

	if (SystemInformationClass == systemKernelDebuggerInformation)
	{
		OutputDebugString(
			L"[SystemKernelDebuggerInformation] The debuggee attempts to detect a kernel debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.E.iii\n");

		SYSTEM_KERNEL_DEBUGGER_INFORMATION* sysKernelInfo = static_cast<SYSTEM_KERNEL_DEBUGGER_INFORMATION*>(
			SystemInformation);
		sysKernelInfo->KernelDebuggerEnabled = FALSE; // fake it ;)
		sysKernelInfo->KernelDebuggerNotPresent = TRUE;
	}

	return status;
}

LONG WINAPI HookNtSetInformationThread(
	_In_ HANDLE ThreadHandle,
	_In_ THREADINFOCLASS ThreadInformationClass,
	_In_ PVOID ThreadInformation,
	_In_ ULONG ThreadInformationLength
)
{
	typedef LONG WINAPI NtSetInformationThread(
		_In_ HANDLE xThreadHandle,
		_In_ THREADINFOCLASS xThreadInformationClass,
		_In_ PVOID xThreadInformation,
		_In_ ULONG xThreadInformationLength
	);
	const auto setInfoThread = reinterpret_cast<NtSetInformationThread*>(GetProcAddress(
		copyNtdll, "NtSetInformationThread"));

	if (ThreadInformationClass == ThreadHideFromDebugger)
	{
		OutputDebugString(
			L"[ThreadHideFromDebugger] The debuggee attempts to hide/escape\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.F.iii\n");
		return 1;
	}
	// .... 
	return setInfoThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);
}

LONG WINAPI HookNtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess,
                                    ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes,
                                    POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ulProcessFlags,
                                    ULONG ulThreadFlags, PRTL_USER_PROCESS_PARAMETERS RtlUserProcessParameters,
                                    LPVOID PsCreateInfo, LPVOID PsAttributeList)
{
	typedef LONG WINAPI xNtCreateUserProcess(PHANDLE xProcessHandle, PHANDLE xThreadHandle,
	                                         ACCESS_MASK xProcessDesiredAccess, ACCESS_MASK xThreadDesiredAccess,
	                                         POBJECT_ATTRIBUTES xProcessObjectAttributes,
	                                         POBJECT_ATTRIBUTES xThreadObjectAttributes, ULONG xulProcessFlags,
	                                         ULONG xulThreadFlags,
	                                         PRTL_USER_PROCESS_PARAMETERS xRtlUserProcessParameters,
	                                         LPVOID xPsCreateInfo, LPVOID xPsAttributeList);

	//const auto createUserProc = reinterpret_cast<xNtCreateUserProcess*>(GetProcAddress(copyNtdll, "NtCreateUserProcess"));

	//const LONG ret = createUserProc(ProcessHandle, ThreadHandle, ProcessDesiredAccess, ThreadDesiredAccess, ProcessObjectAttributes, ThreadObjectAttributes, ulProcessFlags, ulThreadFlags, RtlUserProcessParameters, PsCreateInfo, PsAttributeList);

#define MSG_LEN 0x1000
	TCHAR msg[MSG_LEN]{};
	swprintf_s(msg, L"[NtCreateUserProcess] The debuggee creates a new process:\nPath: ");

	const auto procName = new TCHAR[RtlUserProcessParameters->ImagePathName.MaximumLength]{};
	_tcscpy_s(procName, RtlUserProcessParameters->ImagePathName.Length, RtlUserProcessParameters->ImagePathName.Buffer);
	_tcscat_s(msg, procName);

	_tcscat_s(msg, L"\nCommand Line: ");
	const auto cmdLine = new TCHAR[RtlUserProcessParameters->CommandLine.MaximumLength]{};
	_tcscpy_s(cmdLine, RtlUserProcessParameters->CommandLine.Length, RtlUserProcessParameters->CommandLine.Buffer);
	_tcscat_s(msg, cmdLine);

	OutputDebugString(msg);

	RtlSecureZeroMemory(msg, MSG_LEN);

	TCHAR proc[0x400]{};
	swprintf_s(proc, L"\"%s\"", procName);
	swprintf_s(msg, L"DBG_NEW_PROC:%s %s", proc, cmdLine);
	OutputDebugString(msg);


	return 0; // ???
}

LONG WINAPI HookNtCreateThreadEx(
	_Out_ PHANDLE ThreadHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_ HANDLE ProcessHandle,
	_In_ PVOID StartRoutine, // PUSER_THREAD_START_ROUTINE
	_In_opt_ PVOID Argument,
	_In_ ULONG CreateFlags, // THREAD_CREATE_FLAGS_*
	_In_opt_ ULONG_PTR ZeroBits,
	_In_opt_ SIZE_T StackSize,
	_In_opt_ SIZE_T MaximumStackSize,
	_In_opt_ PVOID AttributeList
)
{
	typedef LONG WINAPI xNtCreateThreadEx(
		_Out_ PHANDLE xThreadHandle,
		_In_ ACCESS_MASK xDesiredAccess,
		_In_opt_ POBJECT_ATTRIBUTES
		xObjectAttributes,
		_In_ HANDLE xProcessHandle,
		_In_ PVOID xStartRoutine,
		// PUSER_THREAD_START_ROUTINE
		_In_opt_ PVOID xArgument,
		_In_ ULONG xCreateFlags,
		// THREAD_CREATE_FLAGS_*
		_In_opt_ ULONG_PTR xZeroBits,
		_In_opt_ SIZE_T xStackSize,
		_In_opt_ SIZE_T xMaximumStackSize,
		_In_opt_ PVOID xAttributeList
	);
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x00000004

	const auto exThread = reinterpret_cast<xNtCreateThreadEx*>(GetProcAddress(copyNtdll, "NtCreateThreadEx"));

	if (CreateFlags == THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
	{
		OutputDebugString(
			L"[NtCreateThreadEx] The debuggee attempts to use THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER flag to hide from us. [FLAG REMOVED]\n\tref: https://goo.gl/4auRMZ \n");

		CreateFlags ^= THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER;
	}

	return exThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags,
	                ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

LONG WINAPI HookNtSystemDebugControl(
	IN DEBUG_CONTROL_CODE Command,
	IN PVOID InputBuffer,
	IN ULONG InputBufferLength,
	OUT PVOID OutputBuffer,
	IN ULONG OutputBufferLength,
	OUT PULONG ReturnLength
)
{
	typedef LONG WINAPI
		xNtSystemDebugControl(
			IN DEBUG_CONTROL_CODE xCommand,
			IN PVOID xInputBuffer,
			IN ULONG xInputBufferLength,
			OUT PVOID xOutputBuffer,
			IN ULONG xOutputBufferLength,
			OUT PULONG xReturnLength
		);

	const auto ntSysDbgCtrl = reinterpret_cast<xNtSystemDebugControl*>(GetProcAddress(copyNtdll, "NtSystemDebugControl")
	);

	if (Command != 0x1d)
	{
		OutputDebugString(
			L"[NtSystemDebugControl] The debuggee attempts to detect a debugger\n\tref: https://goo.gl/j4g5pV \n");
	}

	return ntSysDbgCtrl(Command, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength, ReturnLength);
}

BOOL HookNtYieldExecution()
{
	typedef BOOL xNtYieldExecutionAPI();
	const auto nYExec = reinterpret_cast<xNtYieldExecutionAPI*>(GetProcAddress(copyNtdll, "NtYieldExecutionAPI"));

	OutputDebugStringA(
		"[NtYieldExecutionAPI] Unreliable method for detecting a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.xiii\n");

	return nYExec();
}

LONG WINAPI HookNtSetLdtEntries(ULONG Selector1, LDT_ENTRY Entry1, ULONG Selector2, LDT_ENTRY Entry2)
{
	typedef LONG WINAPI xNtSetLdtEntries(
		ULONG xSelector1,
		LDT_ENTRY xEntry1,
		ULONG xSelector2,
		LDT_ENTRY xEntry2
	);
	const auto ntSetLdt = reinterpret_cast<xNtSetLdtEntries*>(GetProcAddress(copyNtdll, "NtSetLdtEntries"));

	OutputDebugString(
		L"[NtSetLdtEntries] The debugee uses NtSetLdtEntries API\n\tref: https://goo.gl/HaKCfH - 2.1.2\n");

	return ntSetLdt(Selector1, Entry1, Selector2, Entry2);
}

ULONG NTAPI HookNtQueryInformationThread(
	_In_ HANDLE ThreadHandle,
	_In_ THREADINFOCLASS ThreadInformationClass,
	_Out_writes_bytes_(ThreadInformationLength) PVOID ThreadInformation,
	_In_ ULONG ThreadInformationLength,
	_Out_opt_ PULONG ReturnLength
)
{
	typedef ULONG
		NTAPI
		NtQueryInformationThread(
			_In_ HANDLE xThreadHandle,
			_In_ THREADINFOCLASS
			xThreadInformationClass,
			_Out_writes_bytes_(xThreadInformationLength
                                                     ) PVOID
			xThreadInformation,
			_In_ ULONG
			xThreadInformationLength,
			_Out_opt_ PULONG
			xReturnLength
		);
	const auto ntQrInfThr = reinterpret_cast<NtQueryInformationThread*>(GetProcAddress(
		copyNtdll, "NtQueryInformationThread"));


	if (ThreadInformationClass == ThreadHideFromDebugger)
	{
		OutputDebugString(
			L"[NtQueryInformationThread] The debugee attempts to detect anti-anti-debug tool\n\tref: https://goo.gl/k4P2a3 \n");
	}

	return ntQrInfThr(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);
}

BOOL WINAPI HookIsDebuggerPresent()
{
	if (!isDbgCheck)
		OutputDebugString(
			L"[IsDebuggerPresent] The debugee attempts to detect a debugger\n\tref: https://goo.gl/cg7Fkm \n");
	isDbgCheck = TRUE;
	return FALSE; // :)
}

BOOL WINAPI HookCheckRemoteDebuggerPresent(
	_In_ HANDLE hProcess,
	_Inout_ PBOOL pbDebuggerPresent
)
{
	OutputDebugString(
		L"[CheckRemoteDebuggerPresent] The debugee attempts to detect a debugger\n\tref: https://goo.gl/LrUdaG \n");

	typedef BOOL WINAPI xCheckRemoteDebuggerPresent(
		_In_ HANDLE xhProcess,
		_Inout_ PBOOL xpbDebuggerPresent
	);

	if (hProcess == HANDLE(-1))
		return FALSE; // ;)

	const auto chkRemDbg = reinterpret_cast<xCheckRemoteDebuggerPresent*>(GetProcAddress(
		copyKernelBase, "CheckRemoteDebuggerPresent"));
	return chkRemDbg(hProcess, pbDebuggerPresent);
}

ULONG WINAPI HookNtCreateDebugObject(
	OUT PHANDLE DebugObjectHandle,
	IN ACCESS_MASK DesiredAccess,
	IN POBJECT_ATTRIBUTES ObjectAttributes,
	IN ULONG Flags
)
{
	typedef ULONG WINAPI xNtCreateDebugObject(
		OUT PHANDLE xDebugObjectHandle,
		IN ACCESS_MASK xDesiredAccess,
		IN POBJECT_ATTRIBUTES xObjectAttributes,
		IN ULONG xFlags);
	const auto createDbgObj = reinterpret_cast<xNtCreateDebugObject*>(GetProcAddress(copyNtdll, "NtCreateDebugObject"));

	ntCreateDbgObjectCalled = TRUE;

	return createDbgObj(DebugObjectHandle, DesiredAccess, ObjectAttributes, Flags);
}

ULONG WINAPI HookNtQueryObject(
	_In_ HANDLE Handle,
	_In_ OBJECT_INFORMATION_CLASS ObjectInformationClass,
	_Out_writes_bytes_opt_(ObjectInformationLength) PVOID
	ObjectInformation,
	_In_ ULONG ObjectInformationLength,
	_Out_opt_ PULONG ReturnLength
)
{
	typedef ULONG WINAPI xNtQueryObject(
		_In_ HANDLE xHandle,
		_In_ OBJECT_INFORMATION_CLASS
		xObjectInformationClass,
		_Out_writes_bytes_opt_(xObjectInformationLength)
		PVOID
		xObjectInformation,
		_In_ ULONG xObjectInformationLength,
		_Out_opt_ PULONG xReturnLength);

	const auto queryObject = reinterpret_cast<xNtQueryObject*>(GetProcAddress(copyNtdll, "NtQueryObject"));
	const auto Status = queryObject(Handle, ObjectInformationClass, ObjectInformation, ObjectInformationLength,
	                                ReturnLength);

	if (ntCreateDbgObjectCalled && ObjectInformationClass == ObjectTypeInformation)
	{
		OutputDebugString(
			L"[NtQueryObject:ObjectTypeInformation] The debugee attempts to detect a debugger\n\tref: https://goo.gl/krE6JM \n");

		//const auto objectType = static_cast<POBJECT_TYPE_INFORMATION>(ObjectInformation);
		//objectType->TotalNumberOfObjects = 1;  // TODO: crashes on 0x64
	}

	return Status;
}

ULONG WINAPI HookRtlAdjustPrivilege(
	_In_ ULONG Privilege,
	_In_ BOOLEAN Enable,
	_In_ BOOLEAN Client,
	_Out_ PBOOLEAN WasEnabled
)
{
#define SE_DEBUG_PRIVILEGE (20L)

	typedef ULONG WINAPI xRtlAdjustPrivilege(
		_In_ ULONG xPrivilege,
		_In_ BOOLEAN xEnable,
		_In_ BOOLEAN xClient,
		_Out_ PBOOLEAN xWasEnabled
	);
	const auto adjustDbg = reinterpret_cast<xRtlAdjustPrivilege*>(GetProcAddress(copyNtdll, "RtlAdjustPrivilege"));

	const auto Status = adjustDbg(Privilege, Enable, Client, WasEnabled);

	if (Privilege == SE_DEBUG_PRIVILEGE)
	{
		OutputDebugString(
			L"[RtlAdjustPrivilege] The debugee attempts to detect a debugger\n\tref: https://goo.gl/m46tQe \n");
		*WasEnabled = FALSE; // ;)
	}

	return Status;
}

ULONG WINAPI HookNtShutdownSystem(
	IN SHUTDOWN_ACTION /*Action*/)
{
	OutputDebugString(L"[NtShutdownSystem] The debugee attempts to shutdown/reboot system\n");

	return 0; // STATUS_SUCCESS ;)
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hookSetUnhandledExceptionFilter(
	_In_ LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter
)
{
	typedef LPTOP_LEVEL_EXCEPTION_FILTER WINAPI xSetUnhandledExceptionFilter(
		_In_ LPTOP_LEVEL_EXCEPTION_FILTER xlpTopLevelExceptionFilter);
	const auto UnhandledEx = reinterpret_cast<xSetUnhandledExceptionFilter*>(GetProcAddress(
		copyKernelBase, "SetUnhandledExceptionFilter"));

	OutputDebugString(
		L"[SetUnhandledExceptionFilter] [!]Unreliable[!] The debugee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: D.xv\n");

	return UnhandledEx(lpTopLevelExceptionFilter);
}

ULONG WINAPI HookZwAllocateVirtualMemory(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_In_ ULONG_PTR ZeroBits,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG AllocationType,
	_In_ ULONG Protect
)
{
	typedef ULONG WINAPI xZwAllocateVirtualMemory(
		_In_ HANDLE xProcessHandle,
		_Inout_ PVOID* xBaseAddress,
		_In_ ULONG_PTR xZeroBits,
		_Inout_ PSIZE_T xRegionSize,
		_In_ ULONG xAllocationType,
		_In_ ULONG xProtect
	);
	const auto allocVirt = reinterpret_cast<xZwAllocateVirtualMemory*>(GetProcAddress(
		copyNtdll, "ZwAllocateVirtualMemory"));

	allocVirt(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
	memWatchAddress = DWORD_PTR(*BaseAddress);

	if (AllocationType & MEM_WRITE_WATCH)
		memWatch = TRUE;

	return static_cast<ULONG>(memWatchAddress);
}

ULONG WINAPI HookZwGetWriteWatch(
	_In_ HANDLE ProcessHandle,
	_In_ ULONG Flags,
	_In_ PVOID BaseAddress,
	_In_ SIZE_T RegionSize,
	_Out_writes_(*EntriesInUserAddressArray) PVOID* UserAddressArray,
	_Inout_ PULONG_PTR EntriesInUserAddressArray,
	_Out_ PULONG Granularity
)
{
	typedef ULONG
		WINAPI
		xhookZwGetWriteWatch(
			_In_ HANDLE xProcessHandle,
			_In_ ULONG xFlags,
			_In_ PVOID xBaseAddress,
			_In_ SIZE_T xRegionSize,
			_Out_writes_(*xEntriesInUserAddressArray
                                                     ) PVOID*
			xUserAddressArray,
			_Inout_ PULONG_PTR
			xEntriesInUserAddressArray,
			_Out_ PULONG xGranularity
		);

	const auto getWatch = reinterpret_cast<xhookZwGetWriteWatch*>(GetProcAddress(copyNtdll, "ZwGetWriteWatch"));

	if (BaseAddress && memWatch && DWORD_PTR(BaseAddress) == memWatchAddress)
		OutputDebugString(
			L"[WriteWatch] The debugee attempts to detect a debugger [GetWriteWatch]\n\tref: https://goo.gl/jVoMjH \n");

	return getWatch(ProcessHandle, Flags, BaseAddress, RegionSize, UserAddressArray, EntriesInUserAddressArray,
	                Granularity);
}

LONG WINAPI HookRegOpenKeyExInternalW( // not stable !!!!
	_In_ HKEY hKey,
	_In_opt_ LPCTSTR lpSubKey,
	_In_ DWORD ulOptions,
	_In_ REGSAM samDesired,
	_Out_ PHKEY phkResult
#ifndef _WIN64
   ,
   DWORD Unknown // on 0x32 there is 6th parameter
#endif
)
{
	typedef LONG WINAPI RegOpenKeyExW(
		_In_ HKEY xhKey,
		_In_opt_ LPCTSTR xlpSubKey,
		_In_ DWORD xulOptions,
		_In_ REGSAM xsamDesired,
		_Out_ PHKEY xphkResult
	);

	const auto regOpenKeyExW = reinterpret_cast<RegOpenKeyExW*>(GetProcAddress(copyKernelBase, "RegOpenKeyExW"));
	// on 0x64 RegOpenKeyExInternalW crashes


#define OUT_STR_SIZE 0x100
	TCHAR outStr[OUT_STR_SIZE]{}; // ? 
	TCHAR keyHandle[MAX_PATH]{};
	if (hKey)
		switch (reinterpret_cast<ULONG_PTR>(hKey))
		{
		case 0x80000000:
			memcpy_s(keyHandle, MAX_PATH, L"HKEY_CLASSES_ROOT", _countof(L"HKEY_CLASSES_ROOT") * sizeof(TCHAR));
			break;
		case 0x80000001:
			memcpy_s(keyHandle, MAX_PATH, L"HKEY_CURRENT_USER", _countof(L"HKEY_CURRENT_USER") * sizeof(TCHAR));
			break;
		case 0x80000002:
			memcpy_s(keyHandle, MAX_PATH, L"HKEY_LOCAL_MACHINE", _countof(L"HKEY_LOCAL_MACHINE") * sizeof(TCHAR));
			break;
		case 0x80000003:
			memcpy_s(keyHandle, MAX_PATH, L"HKEY_USERS", _countof(L"HKEY_USERS") * sizeof(TCHAR));
			break;
		default:
			break;
		}

	auto keyChecks = jsObject["Registry"]["KeyChecks"];
	TCHAR* normalPath = nullptr;

	if (lpSubKey)
		normalPath = NormalizeRegPath(lpSubKey);
	for (auto& keyArray : keyChecks)
	{
		for (auto& key : keyArray)
		{
			auto strKey = key.get<std::string>();
			std::wstring wstrKey(strKey.begin(), strKey.end());

			if (normalPath && StrStrI(normalPath, wstrKey.c_str()))
			{
				StringCchPrintf(outStr, OUT_STR_SIZE,
				                L"[RegOpenKeyExInternalW] The debugee checks against VM (Vmware, VirtualBox, etc) related registry keys: %s\\%s\n\tref: https://github.com/LordNoteworthy/al-khaser \n",
				                keyHandle, normalPath);

				OutputDebugString(outStr);

				RtlSecureZeroMemory(outStr, OUT_STR_SIZE);

				return ERROR_FILE_NOT_FOUND;
			}
		}
	}

	if (normalPath)
		delete normalPath;

	return regOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

LONG WINAPI HookRegQueryValueExW(
	_In_ HKEY hKey,
	_In_opt_ LPCTSTR lpValueName,
	_Reserved_ LPDWORD lpReserved,
	_Out_opt_ LPDWORD lpType,
	_Out_opt_ LPBYTE lpData,
	_Inout_opt_ LPDWORD lpcbData
)
{
	typedef LONG WINAPI xRegQueryValueEx(
		_In_ HKEY xhKey,
		_In_opt_ LPCTSTR xlpValueName,
		_Reserved_ LPDWORD xlpReserved,
		_Out_opt_ LPDWORD xlpType,
		_Out_opt_ LPBYTE xlpData,
		_Inout_opt_ LPDWORD xlpcbData
	);
	const auto regQueryValueExW = reinterpret_cast<xRegQueryValueEx*>(GetProcAddress(copyKernelBase, "RegQueryValueExW")
	);

	const auto retQueryValue = regQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);

#define OUT_STR_SIZE 0x100
	TCHAR outStr[OUT_STR_SIZE]{}; // ? 

	auto values = jsObject["Registry"]["ValueChecks"];

	for (auto& key : values)
	{
		auto strKey = key.get<std::string>();
		std::wstring wstrKey(strKey.begin(), strKey.end());

		if (lpValueName && StrStrI(lpValueName, wstrKey.c_str()))
		{
			StringCchPrintf(outStr, OUT_STR_SIZE,
			                L"[RegQueryValueExW] The debugee checks following suspicious registry key value - %s:%s\n\tref: https://github.com/LordNoteworthy/al-khaser \n",
			                lpValueName, lpData);
			OutputDebugString(outStr);

			RtlSecureZeroMemory(outStr, OUT_STR_SIZE);
			break;
		}
	}

	return retQueryValue;
}


BOOL WINAPI HookGetThreadContext(
	_In_ HANDLE hThread,
	_Inout_ LPCONTEXT lpContext
)
{
	typedef BOOL WINAPI xGetThreadContext(
		_In_ HANDLE xhThread,
		_Inout_ LPCONTEXT xlpContext
	);

	const auto NtGetThreadContext = reinterpret_cast<xGetThreadContext*>(GetProcAddress(copyNtdll, "ZwGetContextThread")
	);

	const auto ret = NtGetThreadContext(hThread, lpContext);

	lpContext->Dr0 = 0;
	lpContext->Dr1 = 0;
	lpContext->Dr2 = 0;
	lpContext->Dr3 = 0;

	lpContext->Dr6 = 0;
	lpContext->Dr7 = 0;

	return ret;
}

VOID DoWork()
{
#ifdef _DEBUG
#ifndef NO_MSG_BOX
	MessageBox(nullptr, L"DLL injected successfully", L"Debug Mode", MB_ICONINFORMATION);
#endif
#endif

	// save ntdll.dll into temp dir and use it instead of recovering bytes
	GetTempPath(MAX_PATH + 2, tmp);
	TCHAR randNtdll[0x8]{};
	GenRandStr(randNtdll, 0x8);
	_tcscat_s(tmp, randNtdll);
	_tcscat_s(tmp, L".dll");
	GetSystemDirectory(sys, MAX_PATH + 2);
	_tcscat_s(sys, L"\\ntdll.dll");
	CopyFile(sys, tmp, FALSE);
	copyNtdll = LoadLibrary(tmp); // for us ;)
	if (!copyNtdll)
		return;

	//// save kernel*.dll ...
	TCHAR tmp2[MAX_PATH + 2]{};
	TCHAR sys2[MAX_PATH + 2]{};
	GetTempPath(MAX_PATH + 2, tmp2);
	TCHAR randkernelBase[0x10]{};
	GenRandStr(randkernelBase, 0x10);
	_tcscat_s(tmp2, randkernelBase);
	_tcscat_s(tmp2, L".dll");
	GetSystemDirectory(sys2, MAX_PATH + 2);
	_tcscat_s(sys2, L"\\kernelbase.dll");
	CopyFile(sys2, tmp2, FALSE);
	copyKernelBase = LoadLibrary(tmp2); // for us ;)
	if (!copyKernelBase)
		return;

	const auto hFile = CreateFile(L"checks.json", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (INVALID_HANDLE_VALUE != hFile)
	{
		LARGE_INTEGER li;
		GetFileSizeEx(hFile, &li);

		auto chBuffer = std::unique_ptr<char>(new char[li.QuadPart + sizeof(char)]{});
		ReadFile(hFile, chBuffer.get(), li.QuadPart + sizeof(char), nullptr, nullptr);
		jsObject = json::parse(chBuffer.get());
		CloseHandle(hFile);
	}


#if defined(_WIN64)
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
#else
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_ADDRESS_WIDTH_32);
#endif

	// ntdll
	HookFunction("NtClose", DWORD_PTR(HookNtClose), L"ntdll");
	HookFunction("NtOpenProcess", DWORD_PTR(HookNtOpenProcess), L"ntdll");
	HookFunction("NtCreateFile", DWORD_PTR(HookNtCreateFile), L"ntdll");
	HookFunction("NtSetDebugFilterState", DWORD_PTR(HookNtSetDebugFilterState), L"ntdll");
	HookFunction("NtQueryInformationProcess", DWORD_PTR(HookNtQueryInformationProcess), L"ntdll");
	HookFunction("NtQuerySystemInformation", DWORD_PTR(HookNtQuerySystemInformation), L"ntdll");
	HookFunction("NtSetInformationThread", DWORD_PTR(HookNtSetInformationThread), L"ntdll");
	HookFunction("NtCreateUserProcess", DWORD_PTR(HookNtCreateUserProcess), L"ntdll");
	HookFunction("NtCreateThreadEx", DWORD_PTR(HookNtCreateThreadEx), L"ntdll");
	HookFunction("NtSystemDebugControl", DWORD_PTR(HookNtSystemDebugControl), L"ntdll");
	HookFunction("NtYieldExecution", DWORD_PTR(HookNtYieldExecution), L"ntdll");
	HookFunction("NtSetLdtEntries", DWORD_PTR(HookNtSetLdtEntries), L"ntdll");
	HookFunction("NtQueryInformationThread", DWORD_PTR(HookNtQueryInformationThread), L"ntdll");
	HookFunction("NtCreateDebugObject", DWORD_PTR(HookNtCreateDebugObject), L"ntdll");
	HookFunction("NtQueryObject", DWORD_PTR(HookNtQueryObject), L"ntdll");
	HookFunction("RtlAdjustPrivilege", DWORD_PTR(HookRtlAdjustPrivilege), L"ntdll");
	HookFunction("NtShutdownSystem", DWORD_PTR(HookNtShutdownSystem), L"ntdll");
	HookFunction("ZwGetContextThread", DWORD_PTR(HookGetThreadContext), L"ntdll");

	// Causes ZwAllocateVirtualMemory related errors TODO: FIX it
	//hookFunction("ZwAllocateVirtualMemory", DWORD_PTR(hookZwAllocateVirtualMemory), L"ntdll");
	//hookFunction("ZwGetWriteWatch", DWORD_PTR(hookZwGetWriteWatch), L"ntdll");

	// kernelbase
	HookFunction("IsDebuggerPresent", DWORD_PTR(HookIsDebuggerPresent), L"kernelbase");
	HookFunction("CheckRemoteDebuggerPresent", DWORD_PTR(HookCheckRemoteDebuggerPresent), L"kernelbase");
	HookFunction("SetUnhandledExceptionFilter", DWORD_PTR(hookSetUnhandledExceptionFilter), L"kernelbase");
	// what about hooking Rtl version from ntdll?

	// registry checks

	HookFunction("RegOpenKeyExInternalW", DWORD_PTR(HookRegOpenKeyExInternalW), L"kernelbase"); // not stable
	HookFunction("RegQueryValueExW", DWORD_PTR(HookRegQueryValueExW), L"RegQueryValueExW");
	// don't forget "process_output_string" func
}


BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved
)
{
	UNREFERENCED_PARAMETER(hModule);
	UNREFERENCED_PARAMETER(lpReserved);

	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		// Reduce the size of the working set for some applications https://blogs.msdn.microsoft.com/larryosterman/2004/06/03/little-known-win32-apis-disablethreadlibrarycalls/
		DisableThreadLibraryCalls(hModule);

		DoWork();
	}

	return TRUE;
}
