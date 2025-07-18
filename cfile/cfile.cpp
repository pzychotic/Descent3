/*
 * Descent 3
 * Copyright (C) 2024 Parallax Software
 * Copyright (C) 2024–2025 Descent Developers
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "byteswap.h"
#include "crossplat.h"
#include "cfile.h"
#include "default_base_directories.h"
#include "ddio.h"
#include "hogfile.h" //info about library file
#include "log.h"
#include "mem.h"
#include "pserror.h"

// Library structures
struct library_entry {
  char name[PSFILENAME_LEN + 1]; // just the filename part
  uint32_t offset;               // offset into library file
  uint32_t length;               // length of this file
  uint32_t timestamp;            // time and date of file
  uint32_t flags;                // misc flags
};

struct library {
  std::filesystem::path name; // includes path + filename
  uint32_t nfiles = 0;
  std::vector<std::unique_ptr<library_entry>> entries;
  std::shared_ptr<library> next;
  int handle = 0;       // identifier for this lib
  FILE *file = nullptr; // pointer to file for this lib, if no one using it
};

/*
 * List of base directories of the D3 file tree.
 * Directories at the top of the list have higher priority.
 * First entry should be a writable directory.
 */
std::vector<std::filesystem::path> Base_directories = {};

// Map of paths. If value of entry is true, path is only for specific extensions
std::map<std::filesystem::path, bool> paths;

// Map of extensions <=> relevant paths
std::map<std::filesystem::path, std::filesystem::path> extensions;

std::shared_ptr<library> Libraries;
int lib_handle = 0;

// Structure thrown on disk error
cfile_error cfe;
// The message for unexpected end of file
const char *eof_error = "Unexpected end of file";

/* The user can specify a list of default read-only base directories by setting
 * the -DDEFAULT_ADDITIONAL_DIRS CMake option. This function adds those base
 * directories to the list of base directories that the game is currently
 * using.
 */
void cf_AddDefaultBaseDirectories() {
  for (const auto &base_directory : D3::Default_read_only_base_directories) {
    cf_AddBaseDirectory(base_directory);
  }
}

/* This function should be called at least once before you use anything else
 * from this module.
 */
void cf_AddBaseDirectory(const std::filesystem::path &base_directory) {
  if (std::filesystem::exists(base_directory) && std::filesystem::is_directory(base_directory)) {
    Base_directories.push_back(base_directory);
  } else {
    LOG_WARNING << "Ignoring nonexistent base directory: " << base_directory;
  }
}

/* After you call this function, you must call cf_AddBaseDirectory() at least
 * once before you use anything else from this module.
 */
void cf_ClearBaseDirectories() {
  Base_directories.clear();
}


std::filesystem::path cf_LocatePathCaseInsensitiveHelper(const std::filesystem::path &relative_path,
                                                         const std::filesystem::path &starting_dir) {
#ifdef WIN32
  std::filesystem::path result = starting_dir / relative_path;
  if (std::filesystem::exists(result)) {
    return result;
  } else {
    return {};
  }
#else
  // Dumb check, maybe there already all ok?
  if (exists((starting_dir / relative_path))) {
    return starting_dir / relative_path;
  }

  std::filesystem::path result, search_path, search_file;

  search_path = starting_dir / relative_path.parent_path();
  search_file = relative_path.filename();

  // If directory does not exist, nothing to search.
  if (!std::filesystem::is_directory(search_path) || search_file.empty()) {
    return {};
  }


  // Search component in search_path
  auto const &it = std::filesystem::directory_iterator(search_path);

  auto found = std::find_if(it, end(it), [&search_file, &search_path, &result](const auto& dir_entry) {
    return stricmp((const char*)dir_entry.path().filename().u8string().c_str(), (const char*)search_file.u8string().c_str()) == 0;
  });

  if (found != end(it)) {
    // Match, append to result
    result = found->path();
    search_path = result;
  } else {
    // Component not found, mission failed
    return {};
  }

  return result;
#endif
}

std::vector<std::filesystem::path> cf_LocatePathMultiplePathsHelper(const std::filesystem::path &relative_path,
                                                                    bool stop_after_first_result) {
  ASSERT(("realative_path should be a relative path.", relative_path.is_relative()));
  std::vector<std::filesystem::path> return_value = { };
  for (auto base_directories_iterator = Base_directories.rbegin();
       base_directories_iterator != Base_directories.rend();
       ++base_directories_iterator) {
    ASSERT(("base_directory should be an absolute path.", base_directories_iterator->is_absolute()));
    auto to_append = cf_LocatePathCaseInsensitiveHelper(relative_path, *base_directories_iterator);
    ASSERT(("to_append should be either empty or an absolute path.", to_append.empty() || to_append.is_absolute()));
    if (std::filesystem::exists(to_append)) {
      return_value.push_back(to_append);
      if (stop_after_first_result) {
        break;
      }
    }
  }
  return return_value;
}

/**
 * Tries to find a relative path inside of one of the Base_directories.
 *
 * @param relative_path A relative path that we’ll hopefully find in
 *                      one of the Base_directories. You don’t have to get the
 *                      capitalization of relative_path correct, even on macOS
 *                      and Linux.
 *
 * @return Either an absolute path that’s inside a base directory or an empty
 *         path if nothing is found.
 */
std::filesystem::path cf_LocatePath(const std::filesystem::path &relative_path) {
  auto return_value_list = cf_LocatePathMultiplePathsHelper(relative_path, true);
  if (return_value_list.empty()) {
    return "";
  } else {
    return return_value_list.front();
  }
}

/**
 * Tries to find multiple relative paths inside of the Base_directories.
 *
 * @param relative_path A relative path that we’ll hopefully find in
 *                      one or more of the Base_directories. You don’t have to
 *                      get the capitalization of relative_path correct, even on
 *                      macOS and Linux.
 *
 * @return A list of absolute paths. Each path will be inside one of the
 *         Base_directories.
 */
std::vector<std::filesystem::path> cf_LocateMultiplePaths(const std::filesystem::path &relative_path) {
  return cf_LocatePathMultiplePathsHelper(relative_path, false);
}

/* Not all Base_directories are necessarily writable, but this function will
 * return one that should be writable.
 */
std::filesystem::path cf_GetWritableBaseDirectory() {
  return Base_directories.front();
}

// Generates a cfile error
void ThrowCFileError(int type, CFILE *file, const char *msg) {
  cfe.read_write = type;
  cfe.msg = msg;
  cfe.file = file;
  throw &cfe;
}

static void cf_Close();

// searches through the open HOG files, and opens a file if it finds it in any of the libs
static CFILE *open_file_in_lib(const char *filename);

// Opens a HOG file.  Future calls to cfopen(), etc. will look in this HOG.
// Parameters:  libname - path to the HOG file, relative to one of the Base_directories.
// NOTE:	libname must be valid for the entire execution of the program.  Therefore, Base_directories
// 			must not change.
// Returns: 0 if error, else library handle that can be used to close the library
int cf_OpenLibrary(const std::filesystem::path &libname) {
  FILE *fp;
  int i;
  uint32_t offset;
  static int first_time = 1;
  tHogHeader header{};
  tHogFileEntry entry{};

  // allocation library structure
  std::shared_ptr<library> lib = std::make_shared<library>();
  lib->name = cf_LocatePath(libname);
  fp = fopen((const char*)lib->name.u8string().c_str(), "rb");
  if (fp == nullptr) {
    return 0; // CF_NO_FILE;
  }
  // check if this if first library opened
  if (first_time) {
    atexit(cf_Close);
    first_time = 0;
  }
  //	read HOG header
  if (!ReadHogHeader(fp, &header)) {
    fclose(fp);
    return 0; // CF_BAD_LIB;
  }
  lib->nfiles = header.nfiles;
  //	allocate CFILE hog info.
  lib->entries.reserve(lib->nfiles);
  lib->next = Libraries;
  Libraries = lib;
  // set data offset of first file
  offset = header.file_data_offset;
  // Go to index start
  fseek(fp, HOG_HDR_SIZE, SEEK_SET);

  // read in index table
  for (i = 0; i < lib->nfiles; i++) {
    if (!ReadHogEntry(fp, &entry)) {
      fclose(fp);
      return 0;
    }
    // Make sure files are in order
    ASSERT((i == 0) || (stricmp(entry.name, lib->entries[i - 1]->name) >= 0));
    // Copy into table
    std::unique_ptr<library_entry> lib_entry = std::make_unique<library_entry>();
    strcpy(lib_entry->name, entry.name);
    lib_entry->flags = entry.flags;
    lib_entry->length = entry.len;
    lib_entry->offset = offset;
    lib_entry->timestamp = entry.timestamp;
    lib->entries.push_back(std::move(lib_entry));

    offset += entry.len;
  }
  // assign a handle
  lib->handle = ++lib_handle;
  // Save the file pointer
  lib->file = fp;
  // Success.  Return the handle
  return lib->handle;
}

/**
 * Closes a library file.
 * @param handle the handle returned by cf_OpenLibrary()
 */
void cf_CloseLibrary(int handle) {
  std::shared_ptr<library> lib, prev;
  for (lib = Libraries; lib; prev = lib, lib = lib->next) {
    if (lib->handle == handle) {
      if (prev)
        prev->next = lib->next;
      else
        Libraries = lib->next;
      if (lib->file)
        fclose(lib->file);
      return; // successful close
    }
  }
}

// Closes down the CFILE system, freeing up all data, etc.
void cf_Close() {
  std::shared_ptr<library> next;
  while (Libraries) {
    next = Libraries->next;
    Libraries = next;
  }
}

bool cf_SetSearchPath(const std::filesystem::path &path, const std::vector<std::filesystem::path> &ext_list) {
  // Don't add non-existing path into search paths
  if (!std::filesystem::is_directory(path))
    return false;
  // Get & store full path
  paths.insert_or_assign(std::filesystem::absolute(path), !ext_list.empty());
  // Set extensions for this path
  if (!ext_list.empty()) {
    for (auto const &ext : ext_list) {
      if (!ext.empty()) {
        extensions.insert_or_assign(ext, path);
      }
    }
  }
  return true;
}

/**
 * Removes all search paths that have been added by cf_SetSearchPath
 */
void cf_ClearAllSearchPaths() {
  paths.clear();
  extensions.clear();
}

/**
 * Opens a file for reading in a library, given the library id
 * @param filename
 * @param libhandle
 * @return
 */
CFILE *cf_OpenFileInLibrary(const std::filesystem::path &filename, int libhandle) {
  if (libhandle <= 0)
    return nullptr;

  std::shared_ptr<library> lib = Libraries;

  // find the library that we want to use
  while (lib) {
    if (lib->handle == libhandle)
      break;
    lib = lib->next;
  }

  if (nullptr == lib) {
    // couldn't find the library handle
    return nullptr;
  }

  // now do a binary search for the file entry
  int i, first = 0, last = lib->nfiles - 1, c;
  bool found = false;

  do {
    i = (first + last) / 2;
    c = stricmp((const char*)filename.u8string().c_str(), (const char*)lib->entries[i]->name); // compare to current
    if (c == 0) {
      found = true;
      break;
    }
    if (first >= last) // exhausted search
      break;
    if (c > 0) // search key after check key
      first = i + 1;
    else // search key before check key
      last = i - 1;
  } while (true);

  if (!found)
    return nullptr; // file not in library

  // open the file for reading
  FILE *fp;
  int r;
  // See if there's an available FILE
  if (lib->file) {
    fp = lib->file;
    lib->file = nullptr;
  } else {
    fp = fopen((const char*)lib->name.u8string().c_str(), "rb");
    if (!fp) {
      LOG_ERROR.printf("Error opening library <%s> when opening file <%s>; errno=%d.", (const char*)lib->name.u8string().c_str(),
                       (const char*)filename.u8string().c_str(), errno);
      Int3();
      return nullptr;
    }
  }
  auto cfile = mem_rmalloc<CFILE>();
  if (!cfile)
    Error("Out of memory in cf_OpenFileInLibrary()");
  cfile->name = lib->entries[i]->name;
  cfile->file = fp;
  cfile->lib_handle = lib->handle;
  cfile->size = lib->entries[i]->length;
  cfile->lib_offset = lib->entries[i]->offset;
  cfile->position = 0;
  cfile->flags = 0;
  r = fseek(fp, cfile->lib_offset, SEEK_SET);
  ASSERT(r == 0);
  return cfile;
}

// searches through the open HOG files, and opens a file if it finds it in any of the libs
CFILE *open_file_in_lib(const char *filename) {
  CFILE *cfile;
  std::shared_ptr<library> lib = Libraries;
  while (lib) {
    int i;
    // Do binary search for the file
    int first = 0, last = lib->nfiles - 1, c, found = 0;
    do {
      i = (first + last) / 2;
      c = stricmp(filename, lib->entries[i]->name); // compare to current
      if (c == 0) {                                 // found it
        found = 1;
        break;
      }
      if (first >= last) // exhausted search
        break;
      if (c > 0) // search key after check key
        first = i + 1;
      else // search key before check key
        last = i - 1;
    } while (true);
    if (found) {
      FILE *fp;
      int r;
      // See if there's an available FILE
      if (lib->file) {
        fp = lib->file;
        lib->file = nullptr;
      } else {
        fp = fopen((const char*)lib->name.u8string().c_str(), "rb");
        if (!fp) {
          LOG_ERROR.printf("Error opening library <%s> when opening file <%s>; errno=%d.", (const char*)lib->name.u8string().c_str(),
                           filename, errno);
          Int3();
          return nullptr;
        }
      }
      cfile = mem_rmalloc<CFILE>();
      if (!cfile)
        Error("Out of memory in open_file_in_lib()");
      cfile->name = lib->entries[i]->name;
      cfile->file = fp;
      cfile->lib_handle = lib->handle;
      cfile->size = lib->entries[i]->length;
      cfile->lib_offset = lib->entries[i]->offset;
      cfile->position = 0;
      cfile->flags = 0;
      r = fseek(fp, cfile->lib_offset, SEEK_SET);
      ASSERT(r == 0);
      return cfile;
    }
    lib = lib->next;
  }
  return nullptr;
}

// look for the file in the specified directory
static CFILE *open_file_in_directory(const std::filesystem::path &filename, const char *mode,
                                     const std::filesystem::path &directory);

// look for the file in the specified directory
CFILE *open_file_in_directory(const std::filesystem::path &filename, const char *mode,
                              const std::filesystem::path &directory) {
  FILE *fp;
  CFILE *cfile;
  std::filesystem::path using_filename;
  char tmode[3] = "rb";
  if (std::filesystem::is_directory(directory)) {
    // Make a full path
    using_filename = directory / filename;
  } else if (filename.is_absolute()) {
    // no directory specified, and filename is an absolute path
    using_filename = filename;
  } else {
    // no directory specified, and filename is a relative path
    using_filename = cf_LocatePath(filename);
  }

  // set read or write mode
  tmode[0] = mode[0];
  // if mode is "w", then open in text or binary as requested.  If "r", always open in "rb"
  tmode[1] = (mode[0] == 'w') ? mode[1] : 'b';
  // try to open file
  fp = fopen((const char*)using_filename.u8string().c_str(), tmode);

  if (!fp) {
    // File not found
    return nullptr;
  } else {
    using_filename = filename;
  }

  // found the file, open it
  cfile = mem_rmalloc<CFILE>();
  if (!cfile)
    Error("Out of memory in open_file_in_directory()");
  cfile->name = mem_rmalloc<char>((strlen((const char*)using_filename.u8string().c_str()) + 1));
  if (!cfile->name)
    Error("Out of memory in open_file_in_directory()");
  strcpy(cfile->name, (const char*)using_filename.u8string().c_str());
  cfile->file = fp;
  cfile->lib_handle = -1;
  cfile->size = ddio_GetFileLength(fp);
  cfile->lib_offset = 0; // 0 means on disk, not in HOG
  cfile->position = 0;
  cfile->flags = 0;
  return cfile;
}

// Opens a file for reading or writing
// If a path is specified, will try to open the file only in that path.
// If no path is specified, will look through search directories and library files.
// Parameters:	filename - the name if the file, with or without a path
//					mode - the standard C mode string
// Returns:		the CFile handle, or NULL if file not opened
CFILE *cfopen(const std::filesystem::path &filename, const char *mode) {
  CFILE *cfile;

  // Check for valid mode
  ASSERT((mode[0] == 'r') || (mode[0] == 'w'));
  ASSERT((mode[1] == 'b') || (mode[1] == 't'));
  // get the parts of the pathname
  std::filesystem::path path = filename.parent_path();
  std::filesystem::path fname = filename.stem();
  std::filesystem::path ext = filename.extension();

  // if there is a path specified, use it instead of the libraries, search dirs, etc.
  // if the file is writable, just open it, instead of looking in libs, etc.
  if (!path.empty() || (mode[0] == 'w')) {
    // use path specified with file
    cfile = open_file_in_directory(filename, mode, std::filesystem::path());
    goto got_file; // don't look in libs, etc.
  }

  // First look in the directories for this file's extension
  for (auto const &entry : extensions) {
    if (!strnicmp((const char*)entry.first.u8string().c_str(), (const char*)ext.u8string().c_str(), _MAX_EXT)) {
      // found ext
      cfile = open_file_in_directory(filename, mode, entry.second);
      if (cfile) {
        goto got_file;
      }
    }
  }

  // Next look in the general directories
  for (auto const &entry : paths) {
    if (!entry.second) {
      cfile = open_file_in_directory(filename, mode, entry.first);
      if (cfile)
        goto got_file;
    }
  }
  // Lastly, try the hog files
  cfile = open_file_in_lib((const char*)filename.u8string().c_str());
got_file:;
  if (cfile) {
    if (mode[0] == 'w')
      cfile->flags |= CFF_WRITING;
    if (mode[1] == 't')
      cfile->flags |= CFF_TEXT;
  }
  return cfile;
}

// Returns the length of the specified file
// Parameters: cfp - the file pointer returned by cfopen()
uint32_t cfilelength(CFILE *cfp) { return cfp->size; }

// Closes an open CFILE.
// Parameters:  cfile - the file pointer returned by cfopen()
void cfclose(CFILE *cfp) {
  // Either give the file back to the library, or close it
  if (cfp->lib_handle != -1) {
    std::shared_ptr<library> lib;
    for (lib = Libraries; lib; lib = lib->next) {
      if (lib->handle == cfp->lib_handle) { // found the library
        // if library doesn't already have a file, give it this one
        if (lib->file == nullptr) {
          lib->file = cfp->file;
          cfp->file = nullptr;
        }
        break;
      }
    }
  }
  // If the file handle wasn't given back to library, close the file
  if (cfp->file)
    fclose(cfp->file);
  // free the name, if allocated
  if (!cfp->lib_offset)
    mem_free(cfp->name);
  // free the cfile struct
  mem_free(cfp);
}

// Just like stdio fgetc(), except works on a CFILE
// Returns a char or EOF
int cfgetc(CFILE *cfp) {
  int c;
  static uint8_t ch[3] = "\0\0";
  if (cfp->position >= cfp->size)
    return EOF;

  fread(ch, 1, 1, cfp->file);
  c = ch[0];
  // c = getc( cfp->file );
  if (cfeof(cfp))
    c = EOF;
  if (c != EOF) {
    cfp->position++;
    // do special newline handling for text files:
    //  if CR or LF by itself, return as newline
    //  if CR/LF pair, return as newline
    if (cfp->flags & CFF_TEXT) {
      if (c == 10) // return LF as newline
        c = '\n';
      else if (c == 13) { // check for CR/LF pair
        fread(ch, 1, 1, cfp->file);
        int cc = ch[0]; // getc(cfp->file);
        // if (cc != EOF) {
        if (!cfeof(cfp)) {
          if (cc == 10)      // line feed?
            cfp->position++; //..yes, so swallow it
          else {
            // ungetc(cc,cfp->file);	//..no, so put it back
            fseek(cfp->file, -1, SEEK_CUR);
          }
        }
        c = '\n'; // return CR or CR/LF pair as newline
      }
    }
  }
  return c;
}
// Just like stdio fseek(), except works on a CFILE
int cfseek(CFILE *cfp, long offset, int where) {
  int c;
  long goal_position;
  switch (where) {
  case SEEK_SET:
    goal_position = offset;
    break;
  case SEEK_CUR:
    goal_position = cfp->position + offset;
    break;
  case SEEK_END:
    goal_position = cfp->size + offset;
    break;
  default:
    return 1;
  }
  c = fseek(cfp->file, cfp->lib_offset + goal_position, SEEK_SET);
  cfp->position = ftell(cfp->file) - cfp->lib_offset;
  return c;
}

// Just like stdio ftell(), except works on a CFILE
long cftell(CFILE *cfp) { return cfp->position; }

// Returns true if at EOF
int cfeof(CFILE *cfp) { return (cfp->position >= cfp->size); }

// Tells if the file exists
// Returns non-zero if file exists.  Also tells if the file is on disk
//	or in a hog -  See return values in cfile.h
int cfexist(const std::filesystem::path &filename) {
  CFILE *cfp;
  int ret;

  cfp = cfopen(filename, "rb");
  if (!cfp) {              // Didn't get file.  Why?
    if (errno == EACCES)   // File exists, but couldn't open it
      return CFES_ON_DISK; // so say it exists on the disk

    return CFES_NOT_FOUND; // Say we didn't find the file
  }
  ret = cfp->lib_offset ? CFES_IN_LIBRARY : CFES_ON_DISK;
  cfclose(cfp);
  return ret;
}
// Reads the specified number of bytes from a file into the buffer
// DO NOT USE THIS TO READ STRUCTURES.  This function is for byte
// data, such as a string or a bitmap of 8-bit pixels.
// Returns the number of bytes read.
// Throws an exception of type (cfile_error *) if the OS returns an error on read
int cf_ReadBytes(uint8_t *buf, int count, CFILE *cfp) {
  int i;
  const char *error_msg = eof_error; // default error
  ASSERT(!(cfp->flags & CFF_TEXT));
  if (cfp->position + count <= cfp->size) {
    i = fread(buf, 1, count, cfp->file);
    if (i == count) {
      cfp->position += i;
      return i;
    }
    // if not EOF, then get the error message
    if (!feof(cfp->file))
      error_msg = strerror(errno);
  }
  LOG_ERROR.printf("Error reading %d bytes from position %d of file <%s>; errno=%d.", count, cfp->position, cfp->name,
                   errno);
  return 0;
}
// The following functions read numeric vales from a CFILE.  All values are
// stored in the file in Intel (little-endian) format.  These functions
// will convert to big-endian if required.
// These funtions will exit the program with an error if the value
// cannot be read, so do not call these if you don't require the data
// to be present.
// Read and return an integer (32 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on read
int32_t cf_ReadInt(CFILE *cfp, bool little_endian) {
  int32_t i;
  cf_ReadBytes((uint8_t *)&i, sizeof(i), cfp);
  return little_endian ? D3::convert_le(i) : D3::convert_be(i);
}
// Read and return a int16_t (16 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on read
int16_t cf_ReadShort(CFILE *cfp, bool little_endian) {
  int16_t i;
  cf_ReadBytes((uint8_t *)&i, sizeof(i), cfp);
  return little_endian ? D3::convert_le(i) : D3::convert_be(i);
}
// Read and return a byte (8 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on read
int8_t cf_ReadByte(CFILE *cfp) {
  int8_t i;
  cf_ReadBytes((uint8_t *)&i, sizeof(i), cfp);
  return i;
}
// Read and return a float (32 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on read
float cf_ReadFloat(CFILE *cfp) {
  float f;
  cf_ReadBytes((uint8_t *)&f, sizeof(f), cfp);
  return INTEL_FLOAT(f);
}
// Read and return a double (64 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on read
double cf_ReadDouble(CFILE *cfp) {
  double f;
  cf_ReadBytes((uint8_t *)&f, sizeof(f), cfp);
  return D3::convert_le<double>(f);
}
// Reads a string from a CFILE.  If the file is type binary, this
// function reads until a NULL or EOF is found.  If the file is text,
// the function reads until a newline or EOF is found.  The string is always
// written to the destination buffer null-terminated, without the newline.
// Parameters:  buf - where the string is written
//					n - the maximum string length, including the terminating 0
//					cfp - the CFILE pointer
// Returns the number of bytes in the string, before the terminator
// Does not generate an exception on EOF
int cf_ReadString(char *buf, size_t n, CFILE *cfp) {
  int c;
  int count;
  char *bp;
  if (n == 0)
    return -1;
  bp = buf;
  for (count = 0;; count++) {
    c = cfgetc(cfp);
    if (c == EOF) {
      if (!cfeof(cfp)) // not actually at EOF, so must be error
        ThrowCFileError(CFE_READING, cfp, strerror(errno));
      break;
    }

    if ((!(cfp->flags & CFF_TEXT) && (c == 0)) || ((cfp->flags & CFF_TEXT) && (c == '\n')))
      break;           // end-of-string
    if (count < n - 1) // store char if room in buffer
      *bp++ = c;
  }
  *bp = 0; // write terminator
  return count;
}
// Writes the specified number of bytes from a file into the buffer
// DO NOT USE THIS TO WRITE STRUCTURES.  This function is for byte
// data, such as a string or a bitmap of 8-bit pixels.
// Returns the number of bytes written.
// Throws an exception of type (cfile_error *) if the OS returns an error on write
int cf_WriteBytes(const uint8_t *buf, int count, CFILE *cfp) {
  int i;
  if (!(cfp->flags & CFF_WRITING))
    return 0;
  ASSERT(count > 0);
  i = fwrite(buf, 1, count, cfp->file);
  cfp->position += i;
  if (i != count)
    ThrowCFileError(CFE_WRITING, cfp, strerror(errno));
  return i;
}
// Writes a null-terminated string to a file.  If the file is type binary,
// the string is terminated in the file with a null.  If the file is type
// text, the string is terminated with a newline.
// Parameters:	buf - pointer to the string
//					cfp = the CFILE pointer
// Returns the number of bytes written
// Throws an exception of type (cfile_error *) if the OS returns an error on write
int cf_WriteString(CFILE *cfp, const char *buf) {
  int len;
  len = strlen(buf);
  if (len != 0) // write string
    cf_WriteBytes((uint8_t *)buf, len, cfp);
  // Terminate with newline (text file) or NULL (binary file)
  cf_WriteByte(cfp, (cfp->flags & CFF_TEXT) ? '\n' : 0);
  return len + 1;
}

// Just like stdio fprintf(), except works on a CFILE
int cfprintf(CFILE *cfp, const char *format, ...) {
  va_list args;
  int count;
  va_start(args, format);
  count = vfprintf(cfp->file, format, args);
  va_end(args);
  cfp->position += count + 1; // count doesn't include terminator
  return count;
}

// The following functions write numeric vales to a CFILE.  All values are
// stored to the file in Intel (little-endian) format.
// All these throw an exception if there's an error on write.
// Write an integer (32 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on write
void cf_WriteInt(CFILE *cfp, int32_t i) {
  int t = INTEL_INT(i);
  cf_WriteBytes((uint8_t *)&t, sizeof(t), cfp);
}

// Write a int16_t (16 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on write
void cf_WriteShort(CFILE *cfp, int16_t s) {
  int16_t t = INTEL_SHORT(s);
  cf_WriteBytes((uint8_t *)&t, sizeof(t), cfp);
}

// Write a byte (8 bits).
// Throws an exception of type (cfile_error *) if the OS returns an error on write
void cf_WriteByte(CFILE *cfp, int8_t b) {
  if (fputc(b, cfp->file) == EOF)
    ThrowCFileError(CFE_WRITING, cfp, strerror(errno));
  cfp->position++;
  // If text file & writing newline, increment again for LF
  if ((cfp->flags & CFF_TEXT) && (b == '\n')) // check for text mode newline
    cfp->position++;
}

// Write a float (32 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on write
void cf_WriteFloat(CFILE *cfp, float f) {
  float t = INTEL_FLOAT(f);
  cf_WriteBytes((uint8_t *)&t, sizeof(t), cfp);
}

// Write a double (64 bits)
// Throws an exception of type (cfile_error *) if the OS returns an error on write
void cf_WriteDouble(CFILE *cfp, double d) {
  auto t = D3::convert_le<double>(d);
  cf_WriteBytes((uint8_t *)&t, sizeof(t), cfp);
}

// Copies a file.  Returns TRUE if copied ok.  Returns FALSE if error opening either file.
// Throws an exception of type (cfile_error *) if the OS returns an error on read or write
bool cf_CopyFile(const std::filesystem::path &dest, const std::filesystem::path &src, int copytime) {
  CFILE *infile, *outfile;
  if (!stricmp((const char*)dest.u8string().c_str(), (const char*)src.u8string().c_str()))
    return true; // don't copy files if they are the same
  infile = (CFILE *)cfopen(src, "rb");
  if (!infile)
    return false;
  outfile = (CFILE *)cfopen(dest, "wb");
  if (!outfile) {
    cfclose(infile);
    return false;
  }
  int progress = 0;
  int readcount = 0;
#define COPY_CHUNK_SIZE 5000
  uint8_t copybuf[COPY_CHUNK_SIZE];
  while (!cfeof(infile)) {
    // uint8_t c;

    if (progress + COPY_CHUNK_SIZE <= infile->size) {
      readcount = COPY_CHUNK_SIZE;
    } else {
      readcount = infile->size - progress;
    }
    cf_ReadBytes(copybuf, readcount, infile);
    cf_WriteBytes(copybuf, readcount, outfile);
    progress += readcount;
    // c=cf_ReadByte (infile);
    // cf_WriteByte (outfile,c);
  }
  bool nlo = !infile->lib_offset;
  cfclose(infile);
  cfclose(outfile);
  if (nlo && copytime) {
    cf_CopyFileTime(dest, src);
  }
  return true;
}

// Checks to see if two files are different.
// Returns TRUE if the files are different, or FALSE if they are the same.
bool cf_Diff(const std::filesystem::path &a, const std::filesystem::path &b) { return (ddio_FileDiff(a, b)); }

// Copies the file time from one file to another
void cf_CopyFileTime(const std::filesystem::path &dest, const std::filesystem::path &src) {
  ddio_CopyFileTime(dest, src);
}

//	rewinds cfile position
void cf_Rewind(CFILE *fp) {
  if (fp->lib_offset) {
    int r = fseek(fp->file, fp->lib_offset, SEEK_SET);
    ASSERT(r == 0);
  } else {
    rewind(fp->file);
  }
  fp->position = 0;
}

// Calculates a 32-bit CRC for the specified file. a return code of -1 means file note found
#define CRC32_POLYNOMIAL 0xEDB88320L
#define CRC_BUFFER_SIZE 5000

uint32_t cf_CalculateFileCRC(CFILE *infile) {
  int i, j;
  uint8_t crcbuf[CRC_BUFFER_SIZE];
  static bool Cfile_crc_calculated = false;
  static uint32_t CRCTable[256];
  uint32_t crc;
  uint32_t temp1;
  uint32_t temp2;
  uint32_t readlen;

  // Only make the lookup table once
  if (!Cfile_crc_calculated) {
    Cfile_crc_calculated = true;

    for (i = 0; i <= 255; i++) {
      crc = i;
      for (j = 8; j > 0; j--) {
        if (crc & 1)
          crc = (crc >> 1) ^ CRC32_POLYNOMIAL;
        else
          crc >>= 1;
      }
      CRCTable[i] = crc;
    }
  }

  crc = 0xffffffffl;
  while (!cfeof(infile)) {
    if ((infile->size - infile->position) < CRC_BUFFER_SIZE)
      readlen = infile->size - infile->position;
    else
      readlen = CRC_BUFFER_SIZE;
    if (!cf_ReadBytes(crcbuf, readlen, infile)) {
      // Doh, error time!
      Int3();
      return 0xFFFFFFFF;
    }
    for (uint32_t a = 0; a < readlen; a++) {
      temp1 = (crc >> 8) & 0x00FFFFFFL;
      temp2 = CRCTable[((int)crc ^ crcbuf[a]) & 0xff];
      crc = temp1 ^ temp2;
    }
  }

  return crc ^ 0xffffffffl;
}

uint32_t cf_GetfileCRC(const std::filesystem::path &src) {
  CFILE *infile = cfopen(src, "rb");
  if (!infile)
    return 0xFFFFFFFF;

  uint32_t crc = cf_CalculateFileCRC(infile);
  cfclose(infile);

  return crc;
}

int cf_DoForeachFileInLibrary(int handle, const std::filesystem::path &ext,
                               const std::function<void(std::filesystem::path)> &func) {
  auto search_library = Libraries;
  while (search_library && search_library->handle != handle) {
    search_library = search_library->next;
  }
  if (!search_library)
    return 0;
  // Iterate entries on found library
  int result = 0;
  for (const auto &item : search_library->entries) {
    if (stricmp((const char*)std::filesystem::path(item->name).extension().u8string().c_str(), (const char*)ext.u8string().c_str()) == 0) {
      func(item->name);
      result++;
    }
  }
  return result;
}

bool cf_IsFileInHog(const std::filesystem::path &filename, const std::filesystem::path &hogname) {
  std::shared_ptr<library> lib = Libraries;

  while (lib) {
    if (stricmp((const char*)lib->name.u8string().c_str(), (const char*)hogname.u8string().c_str()) == 0) {
      // Now look for filename
      CFILE *cf;
      cf = cf_OpenFileInLibrary(filename, lib->handle);
      if (cf) {
        cfclose(cf);
        return true;
      }
    }
    lib = lib->next;
  }

  return false;
}
