#ifndef SCANNER_H
#define SCANNER_H

#include <string>
#include <map>

namespace scanner {

    // Scan the git history of a repository at repo_path (cloned) for env-like files.
    // Returns a map from file path to commit hash (most recent commit where file appears).
    // depth: number of commits to consider (shallow clone depth).
    std::map<std::string, std::string> scan_repo_history(const std::string& repo_path, int depth);

}

#endif // SCANNER_H