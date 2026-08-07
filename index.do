import { Duration } from "std/time"

import isolated function _env(name: string): Result<string, string> from "native_os.hpp" as doof_os::env
import isolated function _pid(): int from "native_os.hpp" as doof_os::pid
import isolated function _platform(): string from "native_os.hpp" as doof_os::platform
import isolated function _architecture(): string from "native_os.hpp" as doof_os::architecture

export enum ProcessGroupMode {
  Isolated,
  Inherited,
}

import class NativeExecProcess from "native_os.hpp" as NativeExecProcess {
  isolated static spawn(
    command: string,
    args: string[],
    cwd: string | none,
    envKeys: string[],
    envValues: string[],
    inheritEnv: bool,
    withStdin: bool,
    mergeStderrIntoStdout: bool,
    inheritOutput: bool,
    isolatedProcessGroup: bool,
    maxOutputBytes: long | none,
    timeoutNanos: long | none
  ): Result<NativeExecProcess, string>

  isolated nextStdoutChunk(): readonly byte[] | none
  isolated nextStderrChunk(): readonly byte[] | none
  isolated writeStdinText(value: string): Result<none, string>
  isolated closeStdin(): Result<none, string>
  isolated isRunning(): bool
  isolated wait(): Result<int, string>
  isolated runToCompletion(): Result<NativeRunResult, string>
  isolated terminate(signal: int): Result<none, string>
  isolated stdoutOpen(): bool
  isolated stderrOpen(): bool
}

import class NativeRunResult from "native_os.hpp" {
  isolated exitCode(): int
  isolated stdout(): readonly byte[]
  isolated stderr(): readonly byte[]
  isolated stdoutTruncated(): bool
  isolated stderrTruncated(): bool
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
  readonly cwd: string | none = none
  readonly env: Map<string, string> = {}
  readonly inheritEnv: bool = true
  readonly withStdin: bool = true
  readonly mergeStderrIntoStdout: bool = false
  readonly inheritOutput: bool = false
  readonly processGroupMode: ProcessGroupMode = .Isolated
  readonly maxOutputBytes: long | none = none
  readonly timeout: Duration | none = none
}

isolated function spawnNative(command: string, args: string[], options: ExecOptions): Result<NativeExecProcess, string> {
  envKeys: string[] := []
  envValues: string[] := []
  for key, value of options.env {
    envKeys.push(key)
    envValues.push(value)
  }

  let timeoutNanos: long | none = none
  if options.timeout != none {
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
    options.inheritOutput,
    options.processGroupMode == .Isolated,
    options.maxOutputBytes,
    timeoutNanos,
  )
}

class ExecStdoutStream implements Stream<readonly byte[]> {
  process: NativeExecProcess
  let currentValue: readonly byte[] = []

  next(): bool {
    chunk := this.process.nextStdoutChunk()
    if chunk == none {
      return false
    }
    this.currentValue = chunk!
    return true
  }

  value(): readonly byte[] => this.currentValue
}

class ExecStderrStream implements Stream<readonly byte[]> {
  process: NativeExecProcess
  let currentValue: readonly byte[] = []

  next(): bool {
    chunk := this.process.nextStderrChunk()
    if chunk == none {
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

  nextStdoutChunk(): readonly byte[] | none {
    return this.native.nextStdoutChunk()
  }

  nextStderrChunk(): readonly byte[] | none {
    return this.native.nextStderrChunk()
  }

  writeStdinText(value: string): Result<none, string> {
    return this.native.writeStdinText(value)
  }

  closeStdin(): Result<none, string> {
    return this.native.closeStdin()
  }

  isRunning(): bool {
    return this.native.isRunning()
  }

  wait(): Result<int, string> {
    return this.native.wait()
  }

  terminate(signal: int = 15): Result<none, string> {
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
  readonly stdoutTruncated: bool = false
  readonly stderrTruncated: bool = false
}

export isolated function run(command: string, args: string[] = [], options: ExecOptions = ExecOptions {}): Result<ExecResult, string> {
  let proc: NativeExecProcess | none = none
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

  assert(proc != none, "expected Exec.spawn success case to initialize proc")

  return case proc!.runToCompletion() {
    s: Success -> Success {
      value: ExecResult {
        exitCode: s.value.exitCode(),
        stdout: s.value.stdout(),
        stderr: s.value.stderr(),
        stdoutTruncated: s.value.stdoutTruncated(),
        stderrTruncated: s.value.stderrTruncated()
      }
    },
    f: Failure -> Failure {
      error: f.error
    }
  }
}
