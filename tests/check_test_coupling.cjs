#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const root = process.cwd();

const collectFiles = (directory, extensions) => {
  const absoluteDirectory = path.join(root, directory);
  const entries = fs.readdirSync(absoluteDirectory, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const relative = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      files.push(...collectFiles(relative, extensions));
    } else if (extensions.has(path.extname(entry.name))) {
      files.push(relative);
    }
  }
  return files.sort();
};

const syncFiles = collectFiles("tests/sync", new Set([".c", ".h"]));
const asyncFiles = collectFiles("tests/async", new Set([".cpp", ".h"]));

const checks = [
  {
    label: "sync",
    files: syncFiles,
    forbidden: [
      /libxyzm\/async\.h/,
      /\bcardio\b/,
      /\btest_async/,
      /\btest_backend/,
      /\bxyzm_transfer_opts_t\b/,
      /\bxyzm_send_request_t\b/,
      /\bxyzm_receive_request_t\b/,
    ],
  },
  {
    label: "async",
    files: asyncFiles,
    forbidden: [
      /libxyzm\/sync\.h/,
      /\bxyzm_status_t\b/,
      /\btest_sync/,
      /\btest_backend/,
      /\bxyzm_transfer_opts_t\b/,
      /\bxyzm_send_request_t\b/,
      /\bxyzm_receive_request_t\b/,
      /\bwith_backend\b/,
    ],
  },
];

let failed = false;

for (const check of checks) {
  for (const relative of check.files) {
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
      continue;
    }
    const text = fs.readFileSync(absolute, "utf8");
    for (const pattern of check.forbidden) {
      if (pattern.test(text)) {
        console.error(`${check.label} coupling violation: ${relative} matches ${pattern}`);
        failed = true;
      }
    }
  }
}

if (failed) {
  process.exit(1);
}
