#pragma once

#include <string>
#include <map>

namespace scanner {
    // Result of a history scan.
    struct ScanResult {
        // file_path -> commit hash of the most recent commit where the file existed
        std::map<std::string, std::string> file_to_commit;

        // file_path -> true for the paths in file_to_commit whose content at
        // that commit matched secret-shaped patterns (assignment of a
        // non-placeholder value to a key/token/secret/password, or a known
        // credential format such as AWS/GitHub/Stripe tokens, JWTs, private
        // keys). Paths absent here were flagged on name alone: worth a
        // manual review, but ranked below the high-confidence leaks.
        std::map<std::string, bool> likely_secret;

        // Commits whose diff was examined (or whose tree matched the parent,
        // so no diff work was needed).
        size_t commits_considered = 0;

        // Commits skipped because their tree was byte-identical to the parent's.
        size_t commits_skipped = 0;

        // True when the walk was stopped by the depth limit before reaching the
        // start of history.
        bool truncated_by_depth = false;

        // True when the optional early-exit heuristic stopped the walk because
        // `early_exit` consecutive commits produced no new findings.
        bool stopped_early = false;
    };

    // Walk a repository's reachable history and find environment/configuration
    // files (`.env`, *config*, *settings*, *secrets*) that are not inside
    // excluded directories and do not end in a "safe" extension.
    //
    // `depth <= 0` means the entire reachable history; a positive value scans at
    // most that many commits (newest first).
    //
    // `early_exit == 0` disables the heuristic; a positive value stops the walk
    // once that many consecutive commits produced no new findings (only ever
    // applied when the whole history is being scanned).
    //
    // The map records, per matched path, the most recent commit in which the
    // file is known to exist:
    //   - files still present at the newest tip are attributed to that tip;
    //   - files long-deleted are attributed to the parent of the commit that
    //     deleted them (the newest commit in which they still existed).
    ScanResult scan_repo_history(const std::string& repo_path, int depth, size_t early_exit);
}