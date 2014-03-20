#include "Updater.h"

void HashToString(BYTE *in, TCHAR *out)
{
    const char alphabet[] = "0123456789abcdef";

    for (int i = 0; i != 20; i++)
    {
        out[2*i]     = alphabet[in[i] / 16];
        out[2*i + 1] = alphabet[in[i] % 16];
    }

    out[40] = 0;
}

void StringToHash(TCHAR *in, BYTE *out)
{
    int temp;

    for (int i = 0; i < 20; i++)
    {
        _stscanf_s(in + i * 2, _T("%02x"), &temp);
        out[i] = temp;
    }
}

bool CalculateFileHash(TCHAR *path, BYTE *hash)
{
    BYTE buff[65536];
    HCRYPTHASH hHash;

    if (!CryptCreateHash(hProvider, CALG_SHA1, 0, 0, &hHash))
        return false;

    HANDLE hFile;

    hFile = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            //A missing file is OK
            memset (hash, 0, 20);
            return true;
        }

        return true;
    }

    for (;;)
    {
        DWORD read;

        if (!ReadFile(hFile, buff, sizeof(buff), &read, NULL))
        {
            CloseHandle(hFile);
            return false;
        }

        if (!read)
            break;

        if (!CryptHashData(hHash, buff, read, 0))
        {
            CryptDestroyHash(hHash);
            CloseHandle(hFile);
            return false;
        }
    }

    CloseHandle(hFile);

    DWORD hashLength = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLength, 0))
        return false;

    CryptDestroyHash(hHash);
    return true;
}
