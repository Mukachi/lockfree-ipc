// What every benchmark program does around its measurements: name the run
// file, write the environment block first and refuse an unpublishable run,
// write the file. Design: docs/design/004-environment.md
#pragma once

#include <lfipc/bench/environment.hpp>
#include <lfipc/bench/json.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>

namespace lfipc::bench {

// <results_dir>/<name>-<host>-<UTC time>.json
inline std::string default_run_path(const std::string& results_dir, const std::string& name) {
    char host[256] = "unknown";
    gethostname(host, sizeof(host) - 1);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc;
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
    return results_dir + "/" + name + "-" + host + "-" + stamp + ".json";
}

struct run_file {
    std::string path;
    bool published = false;
    bool refused = false;  // published but not in bench mode: measure nothing
    json_writer w;

    // Opens the run's JSON object and writes the environment block (S1).
    run_file(int argc, char** argv, const std::string& results_dir, const std::string& name)
        : path(argc > 1 ? argv[1] : default_run_path(results_dir, name)) {
        published = is_published(path, results_dir);
        const auto env = capture_environment(env_reader{}, argc, argv);
        const auto failed = bench_mode_failures(env);
        refused = published && !failed.empty();
        w.begin_object();
        write_environment_block(w, env, published, failed);
        if (refused) {
            for (const auto& f : failed) std::fprintf(stderr, "%s: not in bench mode: %s\n", name.c_str(), f.c_str());
        }
    }

    // Closes the object and writes the file. Exit status: 0 published or
    // smoke run complete, 1 refused anywhere in the run, 2 cannot write.
    int finish(bool ok, const char* name) {
        w.end_object();
        std::ofstream f(path);
        f << w.str() << '\n';
        f.close();
        if (!f) {
            std::fprintf(stderr, "%s: cannot write %s\n", name, path.c_str());
            return 2;
        }
        std::printf("wrote %s%s\n", path.c_str(), published ? "" : " (not published: outside results/)");
        return ok && !refused ? 0 : 1;
    }
};

}  // namespace lfipc::bench
