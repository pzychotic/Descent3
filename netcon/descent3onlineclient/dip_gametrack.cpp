/*
 * Descent 3
 * Copyright (C) 2024 Descent Developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef WIN32
#include <windows.h>
#endif

#include <filesystem>
#include <string>
#include <sstream>
#include <thread>
#include "dip_gametrack.h"
#include "inetgetfile.h"

#if (defined(LOGGER) && (!defined(RELEASE)))
#define DLLmprintf(...) DLLDebug_ConsolePrintf(__VA_ARGS__)
#else
#define DLLmprintf(...)
#endif

#define DLLddio_MakePath(...) DLLddio_MakePath(__VA_ARGS__)

typedef void (*Debug_ConsolePrintf_fp)(int n, const char *format, ...);
extern Debug_ConsolePrintf_fp DLLDebug_ConsolePrintf;

typedef void (*ddio_MakePath_fp)(char *newPath, const char *absolutePathHeader, const char *subDir, ...);
extern ddio_MakePath_fp DLLddio_MakePath;

char tempTrackerFilename[500];
std::queue<apiServerEntry> directIpHostList;

std::queue<apiServerEntry> GetDIpGameList() {
  std::queue<apiServerEntry> out;
  while (!directIpHostList.empty()) {
    apiServerEntry entry = directIpHostList.front();
    out.push(entry);
    directIpHostList.pop();
  }

  return out;
}

void AddApiHostToDirectIpList(const std::string &str) {
  char ipbuf[16];
  uint16_t iport = 2092;
  const char *address = str.c_str();
  const char *hostport = strchr(address, ':');

  if (hostport) {
    iport = atoi(hostport + 1);
    size_t count = hostport - address;
    if (count > 15)
      return;

    strncpy(ipbuf, address, count);
    ipbuf[count] = '\0';
  } else {
    strncpy(ipbuf, address, 15);
    ipbuf[15] = '\0';
  }

  uint32_t iaddr = inet_addr(ipbuf);

  if (iaddr != INADDR_NONE) {
    apiServerEntry entry = {iaddr, htons(iport)};
    directIpHostList.push(entry);
  }
}

void DecodeApiAnswer(const std::string &data) {
  while (!directIpHostList.empty())
    directIpHostList.pop();

  std::stringstream ss(data);
  while (!ss.eof()) {
    std::string s1;
    getline(ss, s1, '\n');
    AddApiHostToDirectIpList(s1);
  }
}

void FetchApi() {
  DLLmprintf(0, "fetch api.\n");

  InetGetFile *getfile;
  getfile = new InetGetFile(TSETSEFLYAPIURL, tempTrackerFilename);

  bool failed = false;

  while (true) {
    if (getfile->IsFileReceived()) {
      DLLmprintf(0, "got api gameserver list.\n");
      break;
    } else if (getfile->IsFileError()) {
      failed = true;
      DLLmprintf(0, "api download failed.\n");
      break;
    }
  }

  delete getfile;

  if (failed)
    return;

  std::string filecontents;
  FILE *fp = fopen(tempTrackerFilename, "rb");
  if (!fp)
    return;

  fseek(fp, 0, SEEK_END);
  long lengthhack = ftell(fp);
  filecontents.resize(lengthhack);
  fseek(fp, 0, SEEK_SET);

  if (fread((void *)filecontents.data(), 1, lengthhack, fp) != lengthhack) {
    fclose(fp);
    return;
  }
  fclose(fp);

  DecodeApiAnswer(filecontents);

  return;
}

std::thread trackthread;

void RequestDIPGameList() {
  std::error_code ec;
  std::filesystem::path tempPath = std::filesystem::temp_directory_path(ec);
  if (ec) {
    DLLmprintf(0, "Could not find temporary directory: \"%s\"", ec.message().c_str());
    return;
  }

  DLLddio_MakePath(tempTrackerFilename, tempPath.u8string().c_str(), "Descent3", "cache", "odt.tmp", NULL);

  if (trackthread.joinable())
    trackthread.join();

  trackthread = std::thread(FetchApi);
  trackthread.detach();
}

void RequestDIPShutdown() {
  if (trackthread.joinable())
  trackthread.join();
}