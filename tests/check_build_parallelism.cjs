#!/usr/bin/env node

const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const projectDir = path.resolve(__dirname, "..");
const buildScript = path.join(projectDir, "build.sh");
const temporaryDir = fs.mkdtempSync(
    path.join(os.tmpdir(), "libxyzm-build-parallelism-"),
);

try {
  const binDir = path.join(temporaryDir, "bin");
  const logPath = path.join(temporaryDir, "make-invocations.jsonl");
  const makePath = path.join(binDir, "make");

  fs.mkdirSync(binDir);
  fs.writeFileSync(
      makePath,
      [
        "#!/usr/bin/env node",
        "",
        'const fs = require("fs");',
        "",
        "fs.appendFileSync(",
        "    process.env.LIBXYZM_TEST_MAKE_LOG,",
        '    `${JSON.stringify(process.argv.slice(2))}\\n`,',
        ");",
        "",
      ].join("\n"),
  );
  fs.chmodSync(makePath, 0o755);

  execFileSync("sh", [buildScript], {
    cwd: projectDir,
    env: {
      ...process.env,
      PATH: `${binDir}:${process.env.PATH || ""}`,
      LIBXYZM_BUILD_JOBS: "2",
      LIBXYZM_TEST_MAKE_LOG: logPath,
    },
    stdio: "pipe",
  });

  const invocations = fs.readFileSync(logPath, "utf8")
      .trim()
      .split(/\r?\n/)
      .map((line) => JSON.parse(line));

  if (invocations.length !== 7) {
    throw new Error(`expected 7 make invocations, got ${invocations.length}`);
  }
  if (JSON.stringify(invocations[0]) !== JSON.stringify(["clean"])) {
    throw new Error("the first make invocation must clean the build");
  }

  for (const args of invocations.slice(1)) {
    const jobsOptionIndex = args.indexOf("-j");
    if (jobsOptionIndex === -1 || args[jobsOptionIndex + 1] !== "2") {
      throw new Error(
          `make invocation does not use LIBXYZM_BUILD_JOBS: ${args.join(" ")}`,
      );
    }
  }
} finally {
  fs.rmSync(temporaryDir, { recursive: true, force: true });
}
