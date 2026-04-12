#!/usr/bin/env node

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const RZ_PATH = "/usr/bin/rz";
const SZ_PATH = "/usr/bin/sz";
const TIMEOUT_MS = 30_000;

type ChildResult = {
  code: number | null;
  signal: NodeJS.Signals | null;
  stderr: string;
};

const patternedBytes = (length: number, factor: number, salt: number): Buffer => {
  const buffer = Buffer.alloc(length);

  for (let index = 0; index < length; index += 1) {
    buffer[index] = ((index * factor) ^ salt) & 0xff;
  }
  return buffer;
};

const terminatePair = (
    sampleProc: ChildProcessWithoutNullStreams | null,
    foreignProc: ChildProcessWithoutNullStreams | null): void => {
  const processes = [sampleProc, foreignProc].filter(
      (proc): proc is ChildProcessWithoutNullStreams => proc !== null);

  for (const proc of processes) {
    if (proc.exitCode === null && proc.signalCode === null) {
      proc.kill("SIGKILL");
    }
  }
};

const waitProcess = (
    proc: ChildProcessWithoutNullStreams,
    role: string): Promise<ChildResult> =>
  new Promise((resolvePromise, rejectPromise) => {
    let stderr = "";

    proc.stderr.setEncoding("utf8");
    proc.stderr.on("data", (chunk: string) => {
      stderr += chunk;
    });
    proc.once("error", (error) => {
      rejectPromise(new Error(`${role} failed: ${error.message}`));
    });
    proc.once("close", (code, signal) => {
      resolvePromise({ code, signal, stderr });
    });
  });

const describeFailure = (role: string, result: ChildResult): string => {
  const exitText = result.code !== null ?
    `exit code ${result.code}` :
    `signal ${result.signal ?? "unknown"}`;

  return `${role} exited with ${exitText}\nstderr:\n${result.stderr}`;
};

const runExchange = async (
    sampleArgv: string[],
    foreignArgv: string[],
    foreignCwd: string): Promise<void> => {
  const foreignProc = spawn(foreignArgv[0], foreignArgv.slice(1), {
    cwd: foreignCwd,
    stdio: ["pipe", "pipe", "pipe"],
  });
  const sampleProc = spawn(sampleArgv[0], sampleArgv.slice(1), {
    stdio: ["pipe", "pipe", "pipe"],
  });

  sampleProc.stdout.pipe(foreignProc.stdin);
  foreignProc.stdout.pipe(sampleProc.stdin);

  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    terminatePair(sampleProc, foreignProc);
  }, TIMEOUT_MS);

  try {
    const [sampleResult, foreignResult] = await Promise.all([
      waitProcess(sampleProc, "sample"),
      waitProcess(foreignProc, "foreign"),
    ]);

    if (timedOut) {
      throw new Error("sample exchange timed out");
    }
    if (sampleResult.code !== 0) {
      throw new Error(describeFailure("sample", sampleResult));
    }
    if (foreignResult.code !== 0) {
      throw new Error(describeFailure("foreign", foreignResult));
    }
  } finally {
    clearTimeout(timer);
    terminatePair(sampleProc, foreignProc);
  }
};

const assertFileEquals = (expectedPath: string, actualPath: string): void => {
  const expected = readFileSync(expectedPath);
  const actual = readFileSync(actualPath);

  if (!expected.equals(actual)) {
    throw new Error(
        `file mismatch:\n` +
        `  expected: ${expectedPath}\n` +
        `  actual:   ${actualPath}`);
  }
};

const withTempDirs = async (
    sourcePrefix: string,
    receivePrefix: string,
    action: (sourceDir: string, receiveDir: string) => Promise<void>): Promise<void> => {
  const sourceDir = mkdtempSync(join(tmpdir(), sourcePrefix));
  const receiveDir = mkdtempSync(join(tmpdir(), receivePrefix));

  try {
    await action(sourceDir, receiveDir);
  } finally {
    rmSync(sourceDir, { recursive: true, force: true });
    rmSync(receiveDir, { recursive: true, force: true });
  }
};

const runSendCase = async (samplePath: string): Promise<void> =>
  withTempDirs(
      "libxyzm-sample-send-src-",
      "libxyzm-sample-send-dst-",
      async (sourceDir, receiveDir) => {
        const inputPath = join(sourceDir, "input.bin");
        const outputPath = join(receiveDir, "input.bin");

        writeFileSync(inputPath, patternedBytes(600, 11, 0x00));
        await runExchange(
            [samplePath, "send", inputPath],
            [RZ_PATH, "-Z", "-b", "-q", "-y"],
            receiveDir);
        assertFileEquals(inputPath, outputPath);
      });

const runReceiveCase = async (samplePath: string): Promise<void> =>
  withTempDirs(
      "libxyzm-sample-recv-src-",
      "libxyzm-sample-recv-dst-",
      async (sourceDir, receiveDir) => {
        const inputPath = join(sourceDir, "input.bin");
        const outputPath = join(receiveDir, "input.bin");

        writeFileSync(inputPath, patternedBytes(620, 1, 0x96));
        await runExchange(
            [samplePath, "recv", receiveDir],
            [SZ_PATH, "--zmodem", "-b", "-q", "input.bin"],
            sourceDir);
        assertFileEquals(inputPath, outputPath);
      });

const main = async (argv: string[]): Promise<number> => {
  if (argv.length !== 3) {
    console.error(`usage: ${argv[1]} <sample-executable>`);
    return 1;
  }

  const samplePath = resolve(argv[2]);
  if (!existsSync(samplePath)) {
    console.error(`sample executable not found: ${samplePath}`);
    return 1;
  }
  for (const toolPath of [RZ_PATH, SZ_PATH]) {
    if (!existsSync(toolPath)) {
      console.error(`required tool not found: ${toolPath}`);
      return 1;
    }
  }

  await runSendCase(samplePath);
  await runReceiveCase(samplePath);
  console.log(`${samplePath.split("/").pop()}: foreign ZMODEM sample test passed`);
  return 0;
};

void main(process.argv).then(
    (code) => {
      process.exitCode = code;
    },
    (error: Error) => {
      console.error(error.message);
      process.exitCode = 1;
    });
