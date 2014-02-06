/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/

#include "Updater.h"

#include "scopeguard.hpp"

#include <vector>
#include <codecvt>

#include <stdio.h>
#include <string.h>

#include "../lzma/C/7z.h"
#include "../lzma/C/7zAlloc.h"
#include "../lzma/C/7zCrc.h"
#include "../lzma/C/7zFile.h"
#include "../lzma/C/7zVersion.h"

#define MANIFEST_PATH "/updates/org.example.foo.xconfig"
#define TEMP_PATH "/updates/org.example.foo"

#ifndef MANIFEST_PATH
#define MANIFEST_PATH "\\updates\\packages.xconfig"
#endif

#ifndef TEMP_PATH
#define TEMP_PATH "\\updates\\temp"
#endif

using namespace std;

CRITICAL_SECTION updateMutex;

HANDLE cancelRequested;
HANDLE updateThread;
HINSTANCE hinstMain;
HWND hwndMain;
HCRYPTPROV hProvider;

BOOL bExiting;
BOOL updateFailed = FALSE;

BOOL downloadThreadFailure = FALSE;

int totalFileSize = 0;
int completedFileSize = 0;
int completedUpdates = 0;

template <typename T>
void Zero(T &t)
{
    memset(&t, 0, sizeof(T));
}

VOID Status (const _TCHAR *fmt, ...)
{
    _TCHAR str[512];

    va_list argptr;
    va_start(argptr, fmt);

    StringCbVPrintf(str, sizeof(str), fmt, argptr);

    SetDlgItemText(hwndMain, IDC_STATUS, str);

    va_end(argptr);
}

VOID CreateFoldersForPath (_TCHAR *path)
{
    _TCHAR *p = path;

    while (*p)
    {
        if (*p == '\\' || *p == '/')
        {
            *p = 0;
            CreateDirectory (path, NULL);
            *p = '\\';
        }
        p++;
    }
}

BOOL MyCopyFile (_TCHAR *src, _TCHAR *dest)
{
    int err = 0;
    HANDLE hSrc = NULL, hDest = NULL;

    hSrc = CreateFile (src, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto failure;
    }

    hDest = CreateFile (dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hDest == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto failure;
    }

    BYTE buff[65536];
    DWORD read, wrote;

    for (;;)
    {
        if (!ReadFile (hSrc, buff, sizeof(buff), &read, NULL))
        {
            err = GetLastError();
            goto failure;
        }

        if (read == 0)
            break;

        if (!WriteFile (hDest, buff, read, &wrote, NULL))
        {
            err = GetLastError();
            goto failure;
        }

        if (wrote != read)
            goto failure;
    }

    CloseHandle (hSrc);
    CloseHandle (hDest);

    if (err)
        SetLastError (err);

    return TRUE;

failure:
    if (hSrc != INVALID_HANDLE_VALUE)
        CloseHandle (hSrc);
    
    if (hDest != INVALID_HANDLE_VALUE)
        CloseHandle (hDest);

    return FALSE;
}

VOID CleanupPartialUpdates (update_t *updates)
{
    while (updates->next)
    {
        updates = updates->next;

        if (updates->state == STATE_INSTALLED)
        {
            if (updates->previousFile)
            {
                DeleteFile (updates->outputPath);
                MyCopyFile (updates->previousFile, updates->outputPath);
                DeleteFile (updates->previousFile);
            }
            else
            {
                DeleteFile (updates->outputPath);
            }
        }
        else if (updates->state == STATE_DOWNLOADED)
        {
            DeleteFile (updates->tempPath);
        }
    }
}

VOID DestroyUpdateList (update_t *updates)
{
    update_t *next;

    updates = updates->next;
    if (!updates)
        return;

    updates->next = NULL;

    while (updates)
    {
        next = updates->next;

        if (updates->outputPath)
            free(updates->outputPath);
        if (updates->previousFile)
            free(updates->previousFile);
        if (updates->tempPath)
            free(updates->tempPath);
        if (updates->URL)
            free(updates->URL);

        free (updates);

        updates = next;
    }
}

BOOL IsSafeFilename (_TCHAR *path)
{
    const _TCHAR *p;

    p = path;

    if (!*p)
       return FALSE;

    if (_tcsstr(path, _T("..")))
        return FALSE;

    if (*p == '/')
        return FALSE;

    while (*p)
    {
        if (!isalnum(*p) && *p != '.' && *p != '/' && *p != '_' && *p != '-')
            return FALSE;
        p++;
    }

    return TRUE;
}

BOOL IsSafePath (_TCHAR * path)
{
    const _TCHAR *p;

    p = path;

    if (!*p)
        return TRUE;

    if (!isalnum(*p))
        return FALSE;

    while (*p)
    {
        if (*p == '.' || *p == '\\')
            return FALSE;
        p++;
    }
    
    return TRUE;
}

DWORD WINAPI DownloadWorkerThread (VOID *arg)
{
    BOOL foundWork;
    update_t *updates = (update_t *)arg;

    for (;;)
    {
        foundWork = FALSE;

        EnterCriticalSection (&updateMutex);

        while (updates->next)
        {
            int responseCode;

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            {
                LeaveCriticalSection (&updateMutex);
                return 1;
            }

            updates = updates->next;

            if (updates->state != STATE_PENDING_DOWNLOAD)
                continue;

            updates->state = STATE_DOWNLOADING;

            LeaveCriticalSection (&updateMutex);

            foundWork = TRUE;

            if (downloadThreadFailure)
                return 1;

            Status (_T("Downloading %s"), updates->outputPath);

            if (!HTTPGetFile(updates->URL, updates->tempPath, _T("Accept-Encoding: gzip"), &responseCode))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Could not download %s (error code %d)"), updates->outputPath, responseCode);
                return 1;
            }

            if (responseCode != 200)
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: %s (error code %d)"), updates->outputPath, responseCode);
                return 1;
            }

            BYTE downloadHash[20];
            if (!CalculateFileHash(updates->tempPath, downloadHash))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Couldn't verify integrity of %s"), updates->outputPath);
                return 1;
            }

            if (memcmp(updates->hash, downloadHash, 20))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Integrity check failed on %s"), updates->outputPath);
                return 1;
            }

            EnterCriticalSection (&updateMutex);

            updates->state = STATE_DOWNLOADED;
            completedUpdates++;

            LeaveCriticalSection (&updateMutex);
        }

        if (!foundWork)
        {
            LeaveCriticalSection (&updateMutex);
            break;
        }

        if (downloadThreadFailure)
            return 1;
    }

    return 0;
}

BOOL RunDownloadWorkers (int num, update_t *updates)
{
    DWORD threadID;
    HANDLE *handles;

    InitializeCriticalSection (&updateMutex);

    handles = (HANDLE *)malloc (sizeof(*handles) * num);
    if (!handles)
        return FALSE;

    for (int i = 0; i < num; i++)
    {
        handles[i] = CreateThread (NULL, 0, DownloadWorkerThread, updates, 0, &threadID);
        if (!handles[i])
            return FALSE;
    }

    WaitForMultipleObjects (num, handles, TRUE, INFINITE);

    for (int i = 0; i < num; i++)
    {
        DWORD exitCode;
        GetExitCodeThread (handles[i], &exitCode);
        if (exitCode != 0)
            return FALSE;
    }

    return TRUE;
}

DWORD WINAPI UpdateThread (VOID *arg)
{
    DWORD ret = 1;

    update_t updateList = {0};
    update_t *updates = &updateList;

    DEFER{ DestroyUpdateList(&updateList); };

    DEFER{ if (bExiting) ExitProcess(ret); };

    DEFER
    {
        if (!ret)
            return;

        if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            Status(_T("Update aborted."));

        SendDlgItemMessage(hwndMain, IDC_PROGRESS, PBM_SETSTATE, PBST_ERROR, 0);

        SetDlgItemText(hwndMain, IDC_BUTTON, _T("Exit"));
        EnableWindow(GetDlgItem(hwndMain, IDC_BUTTON), TRUE);

        updateFailed = TRUE;
    };

    HANDLE hObsMutex;

    hObsMutex = OpenMutex(SYNCHRONIZE, FALSE, TEXT("OBSMutex"));
    if (hObsMutex)
    {
        HANDLE hWait[2];
        hWait[0] = hObsMutex;
        hWait[1] = cancelRequested;

        int i = WaitForMultipleObjects(2, hWait, FALSE, INFINITE);

        if (i == WAIT_OBJECT_0)
            ReleaseMutex (hObsMutex);

        CloseHandle (hObsMutex);

        if (i == WAIT_OBJECT_0 + 1)
            return ret;
    }

    if (!CryptAcquireContext(&hProvider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Update failed: CryptAcquireContext failure"));
        return ret;
    }

    SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Searching for available updates..."));

    bool bIsPortable = false;

    _TCHAR *cmdLine = (_TCHAR *)arg;
    if (!cmdLine[0])
    {
        Status(_T("Update failed: Missing command line parameters."));
        return ret;
    }

    _TCHAR *channel = _tcschr(cmdLine, ' ');
    if (channel)
    {
        *channel = 0;
        channel++;
    }
    else
    {
        Status(L"Update failed: Missing command line parameters.");
        return ret;
    }

    _TCHAR *p = _tcschr(channel, ' ');
    if (p)
    {
        *p = 0;
        p++;

        if (!_tcscmp(p, _T("Portable")))
            bIsPortable = true;
    }

    const _TCHAR *targetPlatform = cmdLine;

    TCHAR manifestPath[MAX_PATH];
    TCHAR tempPath[MAX_PATH];
    TCHAR lpAppDataPath[MAX_PATH];

    if (bIsPortable)
    {
        GetCurrentDirectory(_countof(lpAppDataPath), lpAppDataPath);
    }
    else
    {
        SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, lpAppDataPath);
        StringCbCat (lpAppDataPath, sizeof(lpAppDataPath), TEXT("\\OBS"));
    }

    StringCbPrintf (manifestPath, sizeof(manifestPath), L"%s" TEXT(MANIFEST_PATH), lpAppDataPath);
    StringCbPrintf (tempPath, sizeof(tempPath), L"%s" TEXT(TEMP_PATH), lpAppDataPath);

    CreateDirectory(tempPath, NULL);

    DEFER{ RemoveDirectory(tempPath); };

    HANDLE hManifest = CreateFile(manifestPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hManifest == INVALID_HANDLE_VALUE)
    {
        Status(TEXT("Update failed: Could not open update manifest"));
        return ret;
    }

    DEFER{ CloseHandle(hManifest); };

    LARGE_INTEGER manifestfileSize;

    if (!GetFileSizeEx(hManifest, &manifestfileSize))
    {
        Status(TEXT("Update failed: Could not check size of update manifest"));
        return ret;
    }

    CHAR *buff = (CHAR *)malloc ((size_t)manifestfileSize.QuadPart + 1);
    if (!buff)
    {
        Status(TEXT("Update failed: Could not allocate memory for update manifest"));
        return ret;
    }

    DEFER{ free(buff); };

    DWORD read;

    if (!ReadFile (hManifest, buff, (DWORD)manifestfileSize.QuadPart, &read, NULL))
    {
        CloseHandle (hManifest);
        Status(TEXT("Update failed: Error reading update manifest"));
        return ret;
    }

    if (read != manifestfileSize.QuadPart)
    {
        Status(_T("Update failed: Failed to read update manifest"));
        return ret;
    }

    buff[read] = 0;

    json_t *root;
    json_error_t error;

    root = json_loads(buff, 0, &error);

    if (!root)
    {
        Status (_T("Update failed: Couldn't parse update manifest: %S"), error.text);
        return ret;
    }

    DEFER{ json_decref(root); };

    if (!json_is_object(root))
    {
        Status(_T("Update failed: Invalid update manifest"));
        return ret;
    }

    //----------------------
    //Parse update manifest
    //----------------------

    char channel_[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, channel, -1, channel_, _countof(channel_), nullptr, nullptr))
        return ret;

    json_t *chan = json_object_get(root, channel_);
    if (!chan)
        return ret;

    json_t *plat = json_object_get(chan, _tcscmp(targetPlatform, L"Win32") ? "win64" : "win");
    if (!plat)
        return ret;

    json_t *hash = json_object_get(plat, "sha1");
    json_t *url = json_object_get(plat, "url");
    json_t *filename = json_object_get(plat, "file");

    if (!json_is_string(hash))
        return ret;

    if (!json_is_string(url))
        return ret;

    if (!json_is_string(filename))
        return ret;

    char const *hash_ = json_string_value(hash);
    char const *url_ = json_string_value(url);
    char const *filename_ = json_string_value(filename);

    _TCHAR w_hash[MAX_PATH];
    _TCHAR w_url[MAX_PATH];
    _TCHAR w_filename[MAX_PATH];
    _TCHAR w_temp_filepath[MAX_PATH];

    if (!MultiByteToWideChar(CP_UTF8, 0, hash_, -1, w_hash, _countof(w_hash)))
        return ret;

    if (!MultiByteToWideChar(CP_UTF8, 0, url_, -1, w_url, _countof(w_url)))
        return ret;

    if (!MultiByteToWideChar(CP_UTF8, 0, filename_, -1, w_filename, _countof(w_filename)))
        return ret;

    StringCbPrintf(w_temp_filepath, sizeof(w_temp_filepath), _T("%s/%s"), tempPath, w_filename);

    updates->next = (update_t *)malloc(sizeof(*updates));
    updates = updates->next;
    Zero(*updates);

    updates->next = nullptr;
    updates->previousFile = nullptr;
    updates->outputPath = _tcsdup(w_filename);
    updates->tempPath = _tcsdup(w_temp_filepath);
    updates->URL = _tcsdup(w_url);
    updates->state = STATE_PENDING_DOWNLOAD;
    StringToHash(w_hash, updates->hash);

    //-------------------
    //Download Updates
    //-------------------
    updates = &updateList;
    if (!RunDownloadWorkers(1, updates))
        return ret;

    DEFER{ if (ret) CleanupPartialUpdates(&updateList); };

    //----------------
    //Install updates
    //----------------
    if (completedUpdates != 1)
        return ret;

    _TCHAR oldFileRenamedPath[MAX_PATH];

    updates = &updateList;
    if (!updates->next)
        return ret;

    updates = updates->next;

    Status(_T("Extracting from %s..."), updates->outputPath);

    updates->previousFile = _tcsdup(updates->tempPath); // clean up archive on success

    CFileInStream archiveStream;
    CLookToRead lookStream;
    CSzArEx db;
    SRes res;
    ISzAlloc allocImp;
    UInt16 *temp = NULL;
    size_t tempSize = 0;

    allocImp.Alloc = [](void*, size_t size) -> void* { if (size) return malloc(size); return nullptr; };
    allocImp.Free = [](void*, void *addr) { free(addr); };

    char tmp_path[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, updates->tempPath, -1, tmp_path, _countof(tmp_path), nullptr, nullptr))
        return ret;

    if (InFile_Open(&archiveStream.file, tmp_path))
    {
        Status(L"Could not open archive");
        return ret;
    }
    {
        DEFER{ File_Close(&archiveStream.file); DeleteFile(w_filename); };

        FileInStream_CreateVTable(&archiveStream);
        LookToRead_CreateVTable(&lookStream, False);

        lookStream.realStream = &archiveStream.s;
        LookToRead_Init(&lookStream);

        CrcGenerateTable();

        SzArEx_Init(&db);
        DEFER{ SzArEx_Free(&db, &allocImp); };
        res = SzArEx_Open(&db, &lookStream.s, &allocImp, &allocImp);

        if (res != SZ_OK)
            return ret;

        UInt32 blockIndex = 0xFFFFFFFF;
        Byte *outBuffer = nullptr;
        size_t outBufferSize = 0;

        DEFER{ IAlloc_Free(&allocImp, outBuffer); };

        _TCHAR w_outfilename[MAX_PATH];
        for (UInt32 i = 0; i < db.db.NumFiles; i++)
        {
            updates->next = (update_t *)malloc(sizeof(*updates));
            updates = updates->next;
            Zero(*updates);

            static_assert(sizeof(_TCHAR) == sizeof(UInt16), "_TCHAR != UInt16");
            SzArEx_GetFileNameUtf16(&db, i, (UInt16*)w_outfilename);

            updates->tempPath = nullptr;
            updates->outputPath = _tcsdup(w_outfilename);
            updates->state = STATE_INVALID;

            size_t offset = 0;
            size_t outSizeProcessed = 0;
            CSzFileItem const *f = db.db.Files + i;
            CSzFile outFile;
            size_t processedSize;

            Status(L"Extracting %s...", w_outfilename);

            res = SzArEx_Extract(&db, &lookStream.s, i,
                &blockIndex, &outBuffer, &outBufferSize,
                &offset, &outSizeProcessed,
                &allocImp, &allocImp);

            if (res != SZ_OK)
            {
                if (res == SZ_ERROR_UNSUPPORTED)
                    Status(L"Archive type is unsupported");
                else if (res == SZ_ERROR_CRC)
                    Status(L"CRC error for file %s", w_outfilename);
                return ret;
            }

            if (f->IsDir)
                continue;

            //Check if we're replacing an existing file or just installing a new one
            if (GetFileAttributes(updates->outputPath) != INVALID_FILE_ATTRIBUTES)
            {
                //Backup the existing file in case a rollback is needed
                StringCbCopy(oldFileRenamedPath, sizeof(oldFileRenamedPath), updates->outputPath);
                StringCbCat(oldFileRenamedPath, sizeof(oldFileRenamedPath), _T(".old"));

                if (!MyCopyFile(updates->outputPath, oldFileRenamedPath))
                {
                    Status(_T("Update failed: Couldn't backup %s (error %d)"), updates->outputPath, GetLastError());
                    return ret;
                }

                if (OutFile_OpenW(&outFile, updates->outputPath))
                {
                    Status(L"Failed to open files '%s'", updates->outputPath);
                    return ret;
                }

                DEFER{ File_Close(&outFile); };

                processedSize = outSizeProcessed;
                if (File_Write(&outFile, outBuffer + offset, &processedSize) != 0 || processedSize != outSizeProcessed)
                {
                    _TCHAR baseName[MAX_PATH];

                    int is_sharing_violation = (GetLastError() == ERROR_SHARING_VIOLATION);

                    StringCbCopy(baseName, sizeof(baseName), updates->outputPath);
                    p = _tcsrchr(baseName, '/');
                    if (p)
                    {
                        p[0] = '\0';
                        p++;
                    }
                    else
                        p = baseName;

                    if (is_sharing_violation)
                        Status(_T("Update failed: %s is still in use. Close all programs and try again."), p);
                    else
                        Status(_T("Update failed: Couldn't update %s (error %d)"), p, GetLastError());
                    return ret;
                }

                DeleteFile(updates->tempPath);

                updates->previousFile = _tcsdup(oldFileRenamedPath);
                updates->state = STATE_INSTALLED;
            }
            else
            {
                //We may be installing into new folders, make sure they exist
                CreateFoldersForPath(updates->outputPath);

                if (OutFile_OpenW(&outFile, updates->outputPath))
                {
                    Status(L"Failed to open files '%s'", updates->outputPath);
                    return ret;
                }

                DEFER{ File_Close(&outFile); };

                processedSize = outSizeProcessed;
                if (File_Write(&outFile, outBuffer + offset, &processedSize) != 0 || processedSize != outSizeProcessed)
                {
                    Status(_T("Update failed: Couldn't install %s (error %d)"), updates->outputPath, GetLastError());
                    return ret;
                }

                DeleteFile(updates->tempPath);

                updates->previousFile = NULL;
                updates->state = STATE_INSTALLED;
            }
        }
    }

    //If we get here, all updates installed successfully so we can purge the old versions
    updates = &updateList;
    while (updates->next)
    {
        updates = updates->next;

        if (updates->previousFile)
            DeleteFile(updates->previousFile);
    }

    Status(_T("Update complete."));

    ret = 0;

    SetDlgItemText(hwndMain, IDC_BUTTON, _T("Launch OBS"));

    return ret;
}

VOID CancelUpdate (BOOL quit)
{
    if (WaitForSingleObject(updateThread, 0) != WAIT_OBJECT_0)
    {
        bExiting = quit;
        SetEvent (cancelRequested);
    }
    else
    {
        PostQuitMessage(0);
    }
}

VOID LaunchOBS ()
{
    _TCHAR cwd[MAX_PATH];
    _TCHAR obsPath[MAX_PATH];

    GetCurrentDirectory(_countof(cwd)-1, cwd);

    StringCbCopy(obsPath, sizeof(obsPath), cwd);
    StringCbCat(obsPath, sizeof(obsPath), _T("\\OBS.exe"));

    SHELLEXECUTEINFO execInfo;

    ZeroMemory(&execInfo, sizeof(execInfo));

    execInfo.cbSize = sizeof(execInfo);
    execInfo.lpFile = obsPath;
    execInfo.lpDirectory = cwd;
    execInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteEx (&execInfo))
        Status(_T("Can't launch OBS: Error %d"), GetLastError());
    else
        ExitProcess (0);
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_INITDIALOG:
            {
                //SendDlgItemMessage (hwnd, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 200));
                return TRUE;
            }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BUTTON)
            {
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    if (WaitForSingleObject(updateThread, 0) == WAIT_OBJECT_0)
                    {
                        if (updateFailed)
                            PostQuitMessage(0);
                        else
                            LaunchOBS ();
                    }
                    else
                    {
                        EnableWindow ((HWND)lParam, FALSE);
                        CancelUpdate (FALSE);
                    }
                }
            }
            return TRUE;

        case WM_CLOSE:
            CancelUpdate (TRUE);
            return TRUE;
    }

    return FALSE;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
{
    INITCOMMONCONTROLSEX icce;

    hinstMain = hInstance;

    icce.dwSize = sizeof(icce);
    icce.dwICC = ICC_PROGRESS_CLASS;

    InitCommonControlsEx(&icce);

    hwndMain = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_UPDATEDIALOG), NULL, DialogProc);

    if (hwndMain)
        ShowWindow(hwndMain, SW_SHOWNORMAL);
    else
        return 1;

    cancelRequested = CreateEvent (NULL, TRUE, FALSE, NULL);

    updateThread = CreateThread (NULL, 0, UpdateThread, lpCmdLine, 0, NULL);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0))
    {
        if(!IsDialogMessage(hwndMain, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return 0;
}