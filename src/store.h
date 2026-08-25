// store.h -- the bin folder: model library and saved configurations.
//
//   loopcore.exe
//   bin/
//     models/     ready-to-run .engine files -- the library proper
//     converted/  the .pt and .onnx sources they were built from
//     config/     named profiles, plus last.cfg for the previous session
//
// The split matters: the library should list things you can actually load,
// not the intermediates. Sources are still kept, because re-exporting from a
// .pt you have since deleted is not a recoverable situation.
//
// Everything is copied rather than referenced, so the library keeps working
// after the original file is moved, renamed, or deleted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace lc::store {

// ------------------------------------------------------------- locations

std::string appDir();      // folder containing loopcore.exe
std::string binDir();      // <app>/bin
std::string modelsDir();   // <app>/bin/models     -- engines
std::string convertedDir();// <app>/bin/converted  -- .pt / .onnx sources
std::string configDir();   // <app>/bin/config
std::string logsDir();     // <app>/bin/logs      -- crash, session, timing

// Creates the tree if it is missing. Safe to call repeatedly.
bool ensureDirs(Log& log);

// ---------------------------------------------------------------- models

struct ModelEntry {
    std::string name;      // file name with extension
    std::string path;      // absolute
    std::string ext;       // lowercase, no dot
    uint64_t    bytes = 0;
    uint64_t    mtime = 0; // FILETIME as a 64-bit value, for sorting
    std::string sizeText;  // pre-formatted, e.g. "18.4 MB"
    std::string dateText;  // pre-formatted, e.g. "2026-08-05 19:42"
};

// Newest first.
std::vector<ModelEntry> listModels();

// Copies `src` into `destDir` unless it is already there. `dstOut` receives
// the new path. Existing files with the same name are kept and a numeric
// suffix is added, so nothing is ever silently overwritten.
bool importInto(const std::string& src, const std::string& destDir,
                std::string& dstOut, Log& log);

// True when the path already sits inside the given folder.
bool isInside(const std::string& path, const std::string& dir);

bool removeModel(const std::string& path, Log& log);

// Renames a model on disk, keeping its extension and its sidecar.
//
// The engine and its .meta file are a pair -- the sidecar records which GPU
// and TensorRT version built it -- so moving one without the other leaves an
// engine that reports nothing about itself.
bool renameModel(const std::string& path, const std::string& newStem, Log& log);

// Class names for a model, if a sidecar list exists: <model>.names,
// <model>.txt, or classes.txt beside it, one name per line. An engine file
// carries no names, so this is the only place they can come from.
std::vector<std::string> loadClassNames(const std::string& modelPath);

// What an engine was built from and on. Written beside the engine as a
// sidecar, because TensorRT cannot be asked any of this once a plan is
// serialised -- and finding out by trying to deserialize means the failure
// arrives as an error code rather than an explanation.
struct EngineMeta {
    bool        valid = false;
    std::string gpuName;
    int         smMajor = 0, smMinor = 0;
    int         trtMajor = 0, trtMinor = 0;
    std::string source;        // the onnx or pt it came from
    std::string builtAt;
    bool        fp16 = false;
    bool        portable = false;   // built for Ampere-and-newer, not just here
    int         inputW = 0, inputH = 0;
    std::string layout;
    double      buildSeconds = 0.0;
    double      smokeMs = 0.0; // one inference, measured right after building
    bool        smokeOk = false;
};

// The .onnx or .pt an engine came from, if a copy of it is sitting in
// bin\converted on this machine. Matched by file name rather than by the
// recorded path, which will be wrong on any machine but the one that built
// it. Empty when nothing matches.
std::string findSourceFor(const std::string& enginePath);

bool saveEngineMeta(const std::string& enginePath, const EngineMeta& m);
EngineMeta loadEngineMeta(const std::string& enginePath);

// ------------------------------------------------------------- profiles

struct Profile {
    std::string name;   // without extension
    std::string path;
};

std::vector<Profile> listProfiles();

std::string profilePath(const std::string& name);   // config/<name>.cfg
std::string lastSessionPath();                      // config/last.cfg

// Writes every setting plus the model currently in use. The write goes to a
// temporary file and is then moved into place, so a machine that loses power
// mid-write leaves the previous config intact rather than a truncated one.
bool saveConfig(const std::string& path, const Config& cfg,
                const std::string& modelPath, Log& log);

// A cheap value that changes whenever any setting does, for deciding when a
// save is worth doing.
uint64_t configFingerprint(const Config& cfg, const std::string& modelPath);

// Missing keys keep their current value, so an older file still loads.
bool loadConfig(const std::string& path, Config& cfg,
                std::string& modelPathOut, Log& log);

bool removeProfile(const std::string& path, Log& log);

// Strips anything that would be awkward in a file name.
std::string sanitiseName(const std::string& in);

} // namespace lc::store
