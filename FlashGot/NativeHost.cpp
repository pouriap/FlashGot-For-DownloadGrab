#include "stdafx.h"
#include <stdio.h>
#include "NativeHost.h"
#include "jute.h"

Pipe::Pipe(char* nm)
{
    name = TEXT(nm);
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
}
bool Pipe::init()
{
    readHandle = CreateNamedPipe(
        name,
        readFlags,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1 /* number of connections */,
        BUF4K /* output buffer size */,
        BUF4K /* input buffer size */,
        0 /* timeout */,
        &saAttr
    );
    if(readHandle == INVALID_HANDLE_VALUE){
        printf("error creating read handle: %d\n", GetLastError());
        return false;
    }

    writeHandle = CreateFile(
        name,
        GENERIC_WRITE,
        0,
        &saAttr,
        OPEN_EXISTING,
        writeFlags,
        NULL
    );
    if(writeHandle == INVALID_HANDLE_VALUE){
        printf("error creating write handle: %d\n", GetLastError());
        return false;
    }

    return true;
}
void Pipe::close()
{
    CloseHandle(readHandle);
    CloseHandle(writeHandle);
}


OutputPipe::OutputPipe(char* nm) : Pipe(nm)
{
    readFlags = 0 | PIPE_ACCESS_INBOUND;
    writeFlags = FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL;
}
bool OutputPipe::write(const char* data, DWORD dataLen)
{
    if(writeHandle == INVALID_HANDLE_VALUE){
        printf("cannot write to invalid handle: %d\n", GetLastError());
        return false;
    }

    DWORD dwWritten;
    BOOL bSuccess = WriteFile(writeHandle, data, dataLen, &dwWritten, NULL);
    if(bSuccess && (dwWritten==dataLen)){
        return true;
    }
    else if(!bSuccess){
        printf("write failed: %d\n", GetLastError());
    }
    return false;
}


InputPipe::InputPipe(char* nm) : Pipe(nm)
{
    readFlags = FILE_FLAG_OVERLAPPED | PIPE_ACCESS_INBOUND;
    writeFlags = 0 | FILE_ATTRIBUTE_NORMAL;
}
bool InputPipe::dataAvailable(){
    DWORD dwAvail;
    for(int i=0; i<10; i++){
        BOOL success = PeekNamedPipe(readHandle, NULL, NULL, NULL, &dwAvail, NULL);
        if( success && dwAvail > 0 ){ return true; }
        //if we didn't get anything then wait
        Sleep(10);
    }
    return false;
}
bool InputPipe::read(CHAR* readBuf, int bufLen, DWORD& dwRead)
{
    if(readHandle == INVALID_HANDLE_VALUE){
        printf("cannot read from invalid handle: %d\n", GetLastError());
        return false;
    }

    BOOL bSuccess = ReadFile(readHandle, readBuf, bufLen, &dwRead, NULL);
    if(!bSuccess){
        printf("read failed: %d\n", GetLastError());
        return false;
    }

    return true;
}


bool Process::create(HANDLE hStdIN, HANDLE hStdOUT, LPSTR cmd, LPSTR args, LPSTR workDir)
{
    //DWORD processFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    DWORD processFlags = CREATE_NO_WINDOW;
    //STARTUPINFOEX startupInfoEx;
    //STARTUPINFO startupInfo = startupInfoEx.StartupInfo;
    STARTUPINFO startupInfo;

    ZeroMemory( &startupInfo, sizeof(STARTUPINFO) );
    ZeroMemory( &procInfo, sizeof(PROCESS_INFORMATION) );

    startupInfo.cb = sizeof(STARTUPINFO);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;

    startupInfo.hStdInput = hStdIN;
    startupInfo.hStdOutput = hStdOUT;
    startupInfo.hStdError = hStdOUT;

    //todo: thread attributes needs vc++
    //todo: support hosts that are scripts like .bat
    BOOL bSuccess = CreateProcess(
        cmd,
        args,            // command line
        NULL,            // process security attributes
        NULL,            // primary thread security attributes
        TRUE,            // handles are inherited
        processFlags,    // creation flags
        NULL,            // use parent's environment
        workDir,         // use parent's current directory
        &startupInfo,   // STARTUPINFO pointer
        &procInfo       // receives PROCESS_INFORMATIN
    );

    CloseHandle(hStdIN);
    CloseHandle(hStdOUT);
    CloseHandle(procInfo.hThread);

    if(!bSuccess){
        printf("process creation failed: %d\n", GetLastError());
        return false;
    }

    return true;
}

void Process::close()
{
    TerminateProcess(procInfo.hProcess, 0);
    CloseHandle(procInfo.hProcess);
}

NativeHost::NativeHost(std::string manifestPath, std::string extensionId) :
    process(),
    hostStdIN("\\\\.\\pipe\\hostStdIN"),
    hostStdOUT("\\\\.\\pipe\\hostStdOUT"),
    manifPath(manifestPath),
    extId(extensionId),
    hostPath("")
{}

bool NativeHost::init()
{
    initHostPath();

    bool success = true;
    success &= hostStdIN.init();
    success &= hostStdOUT.init();

    if(!success){
        return false;
    }

    std::string args = manifPath + " " + extId;
    LPSTR exe = const_cast<char *>(hostPath.c_str());
    LPSTR exeArgs = const_cast<char *>(args.c_str());
    LPSTR workDir = const_cast<char *>(hostDir.c_str());

    success = process.create(hostStdIN.readHandle, hostStdOUT.writeHandle, exe, exeArgs, workDir);

    return success;

}
void NativeHost::initHostPath()
{
    hostPath = "";
    hostDir = "";
    std::ifstream file(manifPath.c_str());
    std::string fileStr = "";
    std::string tmp;

    while (std::getline(file, tmp)){ fileStr += tmp; }
    if(fileStr.length() == 0){
            return;
    }
    //todo: bad json file causes segmentation fault, change json library
    jute::jValue jVal = jute::parser::parse(fileStr);

    // if path is not available in json
    if(jVal["path"].get_type() != jute::JSTRING){
        return;
    }

    std::string path = jVal["path"].as_string();

    char mDrive[_MAX_DRIVE], mDir[_MAX_DIR], mFilename[_MAX_FNAME], mExt[_MAX_EXT];
    //todo: move back to _s version
    //_splitpath_s(manifPath.c_str(), mDrive, _MAX_DRIVE, mDir, _MAX_DIR, mFilename, _MAX_FNAME, mExt, _MAX_EXT);
    _splitpath(manifPath.c_str(), mDrive, mDir, mFilename, mExt);

    char pDrive[_MAX_DRIVE], pDir[_MAX_DIR], pFilename[_MAX_FNAME], pExt[_MAX_EXT];
    //todo: move back to _s version
    //_splitpath_s(path.c_str(), pDrive, _MAX_DRIVE, pDir, _MAX_DIR, pFilename, _MAX_FNAME, pExt, _MAX_EXT);
    _splitpath(path.c_str(), pDrive, pDir, pFilename, pExt);

    // if host path is relative
    if(strcmp(pDrive, "") == 0){
        hostPath.append(mDrive).append(mDir).append(path);
        hostDir.append(mDrive).append(mDir);
    }
    // if path is absolute
    else{
        hostPath.append(path);
        hostDir.append(pDrive).append(pDir);
    }

}
bool NativeHost::sendMessage(const char* json)
{
    int jsonLen = strlen(json);
    int dataLen = jsonLen + 4;
    char* data = new char[dataLen];
    // Native messaging protocol requires message length as a 4-byte integer prepended to the JSON string
    data[0] = char(((jsonLen>>0) & 0xFF));
    data[1] = char(((jsonLen>>8) & 0xFF));
    data[2] = char(((jsonLen>>16) & 0xFF));
    data[3] = char(((jsonLen>>24) & 0xFF));
    // Add the JSON after the length
    int i;
    for(i=0; i<jsonLen; i++){
        data[i+4] = json[i];
    }

    bool success = hostStdIN.write(data, dataLen);
    delete [] data;
    if(!success){
        return false;
    }

    if(!hostStdOUT.dataAvailable()){
        printf("no data available");
        return false;
    }

    DWORD dwRead;
    CHAR chBuf[BUF2K];
    success = hostStdOUT.read(chBuf, BUF2K, dwRead);
    if(!success){
        return false;
    }

    //todo: remove in production
    //remove first four bytes of size data
    for(i=0; i<dwRead-4; i++){
        chBuf[i] = chBuf[i+4];
    }
    chBuf[i] = '\n';
    HANDLE hParentStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    success = WriteFile(hParentStdOut, chBuf, dwRead-4+1, NULL, NULL);
    if(!success){
        printf("redirecting failed: %d\n", GetLastError());
        return false;
    }

    return true;
}
void NativeHost::close()
{
    hostStdIN.close();
    hostStdOUT.close();
    process.close();
}
