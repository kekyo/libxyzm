#!/usr/bin/env node

const { execFileSync } = require("child_process");

const [, , binaryPath, profile] = process.argv;

if (!binaryPath || !profile) {
  console.error("usage: check_link_scope.cjs <binary> <profile>");
  process.exit(2);
}

const nmOptions = {
  encoding: "utf8",
  maxBuffer: 256 * 1024 * 1024,
};
const output = execFileSync("nm", ["--defined-only", binaryPath], nmOptions);
const demangledOutput = execFileSync("nm", ["-C", "--defined-only", binaryPath], nmOptions);
const rawSymbols =
  output
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.length > 0)
      .map((line) => line.split(/\s+/).pop());
const symbols = new Set(rawSymbols);
for (const symbol of rawSymbols) {
  if (symbol.startsWith("_") && !symbol.startsWith("__")) {
    symbols.add(symbol.substring(1));
  }
}
const demangledSymbols = demangledOutput
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0)
    .map((line) => line.replace(/^([0-9A-Fa-f]+)?\s*[A-Za-z]\s+/, ""));

const profiles = {
  "sync-zmodem-send": {
    present: [
      "xyzm_zmodem_send",
      "xyzm_zmodem_send_impl",
    ],
    absent: [
      "xyzm_zmodem_receive",
      "xyzm_zmodem_receive_impl",
      "xyzm_xmodem_send",
      "xyzm_xmodem_receive",
      "xyzm_ymodem_send",
      "xyzm_ymodem_receive",
    ],
  },
  "sync-tests": {
    present: [
      "xyzm_xmodem_send",
      "xyzm_xmodem_receive",
      "xyzm_ymodem_send",
      "xyzm_ymodem_receive",
      "xyzm_zmodem_send",
      "xyzm_zmodem_receive",
    ],
    absent: [],
    absentDemangled: [
      "xyzm_xmodem_send_async",
      "xyzm_xmodem_receive_async",
      "xyzm_ymodem_send_async",
      "xyzm_ymodem_receive_async",
      "xyzm_zmodem_send_async",
      "xyzm_zmodem_receive_async",
      "cardio::",
    ],
  },
  "async-tests": {
    presentDemangled: [
      "xyzm_xmodem_send_async",
      "xyzm_xmodem_receive_async",
    ],
    absent: [
      "xyzm_xmodem_send",
      "xyzm_xmodem_receive",
      "xyzm_ymodem_send",
      "xyzm_ymodem_receive",
      "xyzm_zmodem_send",
      "xyzm_zmodem_receive",
      "test_sync_send",
      "test_sync_receive",
      "test_sync_send_observed",
      "test_sync_receive_observed",
    ],
  },
};

const config = profiles[profile];

if (!config) {
  console.error(`unknown profile: ${profile}`);
  process.exit(2);
}

for (const symbol of config.present || []) {
  if (!symbols.has(symbol)) {
    console.error(`missing required symbol: ${symbol}`);
    process.exit(1);
  }
}

for (const symbol of config.presentDemangled || []) {
  if (!demangledSymbols.some((candidate) => candidate.includes(symbol))) {
    console.error(`missing required demangled symbol: ${symbol}`);
    process.exit(1);
  }
}

for (const symbol of config.absent || []) {
  if (symbols.has(symbol)) {
    console.error(`unexpected symbol present: ${symbol}`);
    process.exit(1);
  }
}

for (const symbol of config.absentDemangled || []) {
  if (demangledSymbols.some((candidate) => candidate.includes(symbol))) {
    console.error(`unexpected demangled symbol present: ${symbol}`);
    process.exit(1);
  }
}
