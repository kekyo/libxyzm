#!/usr/bin/env node

import { spawn, type ChildProcess } from "node:child_process";
import { createWriteStream, writeFileSync } from "node:fs";
import net, { type Server, type Socket } from "node:net";
import { once } from "node:events";

type SocketReader = {
  pending: Buffer;
  socket: Socket;
};

const BRIDGE_MAGIC = Buffer.from("SPWN", "ascii");

const bridgeLog = (message: string): void => {
  const logPath = process.env.XYZM_WINE_FOREIGN_BRIDGE_LOG;

  if (logPath === undefined || logPath === "") {
    return;
  }
  const stream = createWriteStream(logPath, { flags: "a", encoding: "utf8" });

  stream.write(`${message}\n`);
  stream.end();
};

const createSocketReader = (socket: Socket): SocketReader => ({
  pending: Buffer.alloc(0),
  socket,
});

const waitReadable = async (socket: Socket): Promise<void> => {
  if (socket.readableEnded) {
    throw new Error("unexpected EOF");
  }
  await once(socket, "readable");
};

const readExact = async (reader: SocketReader, length: number): Promise<Buffer> => {
  while (reader.pending.length < length) {
    const chunk = reader.socket.read() as Buffer | null;

    if (chunk !== null) {
      reader.pending = Buffer.concat([reader.pending, chunk]);
      continue;
    }
    await waitReadable(reader.socket);
  }

  const value = reader.pending.subarray(0, length);

  reader.pending = reader.pending.subarray(length);
  return value;
};

const readU32 = async (reader: SocketReader): Promise<number> =>
  (await readExact(reader, 4)).readUInt32BE(0);

const writeU32 = (socket: Socket, value: number): void => {
  const buffer = Buffer.alloc(4);

  buffer.writeUInt32BE(value >>> 0, 0);
  socket.write(buffer);
};

const readText = async (
    reader: SocketReader,
    size: number | undefined): Promise<string> => {
  const actualSize = size ?? await readU32(reader);

  bridgeLog(`readText: size=${actualSize}`);
  if (actualSize === 0) {
    return "";
  }
  return (await readExact(reader, actualSize)).toString("utf8");
};

const wineToUnixPath = (path: string): string | undefined => {
  if (path === "") {
    return undefined;
  }
  if (path.length >= 3 &&
      path[1] === ":" &&
      (path[0] === "Z" || path[0] === "z") &&
      (path[2] === "\\" || path[2] === "/")) {
    const suffix = path.slice(2).replaceAll("\\", "/");

    return `/${suffix.replace(/^\/+/, "")}`;
  }
  return path.replaceAll("\\", "/");
};

const openListener = async (): Promise<{ port: number; server: Server }> => {
  const server = net.createServer();

  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  return {
    port: (server.address() as net.AddressInfo).port,
    server,
  };
};

const acceptSocket = async (server: Server): Promise<Socket> => {
  const [socket] = await once(server, "connection") as [Socket];

  socket.setNoDelay(true);
  return socket;
};

const waitChild = async (
    child: ChildProcess): Promise<{
      exitCode: number;
      stderr: Buffer;
    }> => {
  const stderrChunks: Buffer[] = [];

  if (child.stderr !== null) {
    child.stderr.on("data", (chunk: Buffer | string) => {
      stderrChunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
    });
  }
  const [exitCode, signal] = await once(child, "close") as [
    number | null,
    NodeJS.Signals | null,
  ];

  return {
    exitCode: exitCode ?? (signal === null ? 0 : 127),
    stderr: Buffer.concat(stderrChunks),
  };
};

const bridgeTransport = (transportSocket: Socket, child: ChildProcess): void => {
  if (child.stdin === null || child.stdout === null) {
    throw new Error("child stdio pipes are unavailable");
  }

  // Do not pass the raw transport socket FD to the foreign CLI.
  //
  // Node sockets are used in nonblocking mode, and a duplicated descriptor
  // would keep that open-file-description state in the child process too.
  // Tools such as rz/sz/rx/sx expect blocking stdio and can fail with EAGAIN
  // when they inherit a nonblocking socket. Keep the child on ordinary pipe
  // stdio and relay bytes at the bridge boundary instead.
  transportSocket.on("data", (chunk: Buffer) => {
    if (!child.stdin!.write(chunk)) {
      transportSocket.pause();
    }
  });
  child.stdin.on("drain", () => {
    transportSocket.resume();
  });
  child.stdout.on("data", (chunk: Buffer) => {
    if (!transportSocket.write(chunk)) {
      child.stdout!.pause();
    }
  });
  transportSocket.on("drain", () => {
    child.stdout!.resume();
  });
  transportSocket.on("end", () => {
    child.stdin!.end();
  });
  child.stdout.on("end", () => {
    transportSocket.end();
  });
  transportSocket.on("close", () => {
    child.stdin!.destroy();
    child.stdout!.destroy();
  });
  transportSocket.on("error", () => {
    child.stdin!.destroy();
    child.stdout!.destroy();
  });
  child.stdin.on("error", () => {
    transportSocket.destroy();
  });
  child.stdout.on("error", () => {
    transportSocket.destroy();
  });
};

const handleSpawn = async (reader: SocketReader): Promise<void> => {
  const controlSocket = reader.socket;
  const cwdLength = await readU32(reader);
  const argc = await readU32(reader);
  const cwd = await readText(reader, cwdLength);
  const argv: string[] = [];

  bridgeLog("handleSpawn: begin");
  bridgeLog(`handleSpawn: cwd=${JSON.stringify(cwd)}`);
  bridgeLog(`handleSpawn: argc=${argc}`);
  for (let index = 0; index < argc; index += 1) {
    const arg = await readText(reader, undefined);

    argv.push(arg);
    bridgeLog(`handleSpawn: argv[${index}]=${JSON.stringify(arg)}`);
  }

  const transportListener = await openListener();
  const statusListener = await openListener();

  writeU32(controlSocket, transportListener.port);
  writeU32(controlSocket, statusListener.port);
  controlSocket.end();
  bridgeLog(
      `handleSpawn: waiting transport=${transportListener.port} status=${statusListener.port}`);

  const transportSocket = await acceptSocket(transportListener.server);
  const statusSocket = await acceptSocket(statusListener.server);

  transportListener.server.close();
  statusListener.server.close();
  bridgeLog("handleSpawn: both sockets accepted");

  let exitCode = 127;
  let stderr = Buffer.alloc(0);

  try {
    const child = spawn(argv[0], argv.slice(1), {
      cwd: wineToUnixPath(cwd),
      stdio: ["pipe", "pipe", "pipe"],
    });

    bridgeLog(`handleSpawn: child pid=${child.pid ?? -1}`);
    bridgeTransport(transportSocket, child);
    ({ exitCode, stderr } = await waitChild(child));
    bridgeLog(`handleSpawn: child exit=${exitCode}`);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);

    stderr = Buffer.from(message, "utf8");
    bridgeLog(`handleSpawn: exception=${message}`);
  } finally {
    transportSocket.destroy();
  }

  writeU32(statusSocket, exitCode >>> 0);
  writeU32(statusSocket, stderr.length);
  if (stderr.length > 0) {
    statusSocket.write(stderr);
  }
  statusSocket.end();
  bridgeLog(
      `handleSpawn: status sent exit=${exitCode} stderr_len=${stderr.length}`);
};

const main = async (argv: string[]): Promise<number> => {
  if (argv.length !== 3) {
    console.error(`usage: ${argv[1]} <port-file>`);
    return 2;
  }

  const portFile = argv[2];
  const server = net.createServer();

  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const port = (server.address() as net.AddressInfo).port;

  writeFileSync(portFile, `${port}\n`, "utf8");
  bridgeLog(`main: listening port=${port}`);

  for (;;) {
    const socket = await acceptSocket(server);

    try {
      bridgeLog("main: control accepted");
      const reader = createSocketReader(socket);
      const magic = await readExact(reader, BRIDGE_MAGIC.length);

      // Reuse the same buffered reader after the SPWN preamble. socket.read()
      // may over-read beyond the request magic, so any pending bytes must stay
      // attached to the subsequent length-prefixed control payload parser.
      if (!magic.equals(BRIDGE_MAGIC)) {
        throw new Error(`unexpected request: ${JSON.stringify(magic.toString("latin1"))}`);
      }
      bridgeLog("main: received SPWN");
      await handleSpawn(reader);
    } catch (error) {
      socket.destroy();
      throw error;
    }
  }
};

void main(process.argv).then(
    (code) => {
      process.exitCode = code;
    },
    (error: Error) => {
      console.error(error.message);
      process.exitCode = 1;
    });
