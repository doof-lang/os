import { Duration } from "std/time"

import function _env(name: string): Result<string, string> from "native_os.hpp" as doof_os::env
import function _pid(): int from "native_os.hpp" as doof_os::pid
import function _platform(): string from "native_os.hpp" as doof_os::platform
import function _architecture(): string from "native_os.hpp" as doof_os::architecture

import class NativeExecProcess from "native_os.hpp" as NativeExecProcess {
  static spawn(
    command: string,
    args: string[],
    cwd: string | null,
    envKeys: string[],
    envValues: string[],
    inheritEnv: bool,
    withStdin: bool,
    mergeStderrIntoStdout: bool,
    timeoutNanos: long | null
  ): Result<NativeExecProcess, string>

  nextStdoutChunk(): readonly byte[] | null
  nextStderrChunk(): readonly byte[] | null
  writeStdinText(value: string): Result<void, string>
  closeStdin(): Result<void, string>
  isRunning(): bool
  wait(): Result<int, string>
  runToCompletion(): Result<NativeRunResult, string>
  terminate(signal: int): Result<void, string>
  stdoutOpen(): bool
  stderrOpen(): bool
}

import class NativeRunResult from "native_os.hpp" {
  exitCode(): int
  stdout(): readonly byte[]
  stderr(): readonly byte[]
}

export function env(name: string): Result<string, string> {
  return _env(name)
}

export function pid(): int {
  return _pid()
}

export function platform(): string {
  return _platform()
}

export function architecture(): string {
  return _architecture()
}

export class ExecOptions {
  readonly cwd: string | null = null
  readonly env: Map<string, string> = {}
  readonly inheritEnv: bool = true
  readonly withStdin: bool = true
  readonly mergeStderrIntoStdout: bool = false
  readonly timeout: Duration | null = null
}

function spawnNative(command: string, args: string[], options: ExecOptions): Result<NativeExecProcess, string> {
  envKeys: string[] := []
  envValues: string[] := []
  for key, value of options.env {
    envKeys.push(key)
    envValues.push(value)
  }

  let timeoutNanos: long | null = null
  if options.timeout != null {
    timeoutNanos = options.timeout!.toNanos()
  }

  return NativeExecProcess.spawn(
    command,
    args,
    options.cwd,
    envKeys,
    envValues,
    options.inheritEnv,
    options.withStdin,
    options.mergeStderrIntoStdout,
    timeoutNanos,
  )
}

class ExecStdoutStream implements Stream<readonly byte[]> {
  process: NativeExecProcess
  currentValue: readonly byte[] = []

  next(): bool {
    chunk := this.process.nextStdoutChunk()
    if chunk == null {
      return false
    }
    this.currentValue = chunk!
    return true
  }

  value(): readonly byte[] => this.currentValue
}

class ExecStderrStream implements Stream<readonly byte[]> {
  process: NativeExecProcess
  currentValue: readonly byte[] = []

  next(): bool {
    chunk := this.process.nextStderrChunk()
    if chunk == null {
      return false
    }
    this.currentValue = chunk!
    return true
  }

  value(): readonly byte[] => this.currentValue
}

export class Exec {
  private readonly native: NativeExecProcess

  static spawn(command: string, args: string[] = [], options: ExecOptions = ExecOptions {}): Result<Exec, string> {
    return case spawnNative(command, args, options) {
      s: Success -> Success {
        value: Exec {
          native: s.value
        }
      },
      f: Failure -> Failure {
        error: f.error
      }
    }
  }

  stdoutStream(): Stream<readonly byte[]> {
    return ExecStdoutStream {
      process: this.native
    }
  }

  stderrStream(): Stream<readonly byte[]> {
    return ExecStderrStream {
      process: this.native
    }
  }

  nextStdoutChunk(): readonly byte[] | null {
    return this.native.nextStdoutChunk()
  }

  nextStderrChunk(): readonly byte[] | null {
    return this.native.nextStderrChunk()
  }

  writeStdinText(value: string): Result<void, string> {
    return this.native.writeStdinText(value)
  }

  closeStdin(): Result<void, string> {
    return this.native.closeStdin()
  }

  isRunning(): bool {
    return this.native.isRunning()
  }

  wait(): Result<int, string> {
    return this.native.wait()
  }

  terminate(signal: int = 15): Result<void, string> {
    return this.native.terminate(signal)
  }

  stdoutOpen(): bool {
    return this.native.stdoutOpen()
  }

  stderrOpen(): bool {
    return this.native.stderrOpen()
  }
}

export class ExecResult {
  readonly exitCode: int
  readonly stdout: readonly byte[]
  readonly stderr: readonly byte[]
}

export function run(command: string, args: string[] = [], options: ExecOptions = ExecOptions {}): Result<ExecResult, string> {
  let proc: NativeExecProcess | null = null
  case spawnNative(command, args, options) {
    s: Success -> {
      proc = s.value
    }
    f: Failure -> {
      return Failure {
        error: f.error
      }
    }
  }

  assert(proc != null, "expected Exec.spawn success case to initialize proc")

  case proc!.runToCompletion() {
    s: Success -> {
      nativeResult := s.value
      return Success {
        value: ExecResult {
          exitCode: nativeResult.exitCode(),
          stdout: nativeResult.stdout(),
          stderr: nativeResult.stderr()
        }
      }
    }
    f: Failure -> {
      return Failure {
        error: f.error
      }
    }
  }
}
