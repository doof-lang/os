import { BlobBuilder, BlobReader } from "std/blob"
import { tempDirectory } from "std/path"
import { Duration } from "std/time"
import { Exec, ExecOptions, ExecResult, ProcessGroupMode, architecture, env, pid, platform, run } from "../index"

function bytesToString(data: readonly byte[]): string {
  return BlobReader(data).readString(long(data.length))
}

function collectStream(stream: Stream<readonly byte[]>): readonly byte[] {
  builder := BlobBuilder()
  for chunk of stream {
    builder.writeBytes(chunk)
  }
  return builder.build()
}

function assertSuccessVoid(result: Result<none, string>, context: string): none {
  case result {
    _: Success -> {}
    f: Failure -> {
      assert(false, context + ": " + f.error)
    }
  }
}

export function testEnvReadsKnownVariable(): none {
  home := env("HOME")
  let value = ""
  case home {
    s: Success -> {
      value = s.value
    }
    f: Failure -> {
      assert(false, "expected HOME to be present and non-empty: " + f.error)
    }
  }
  assert(value.length > 0, "expected HOME to be present and non-empty")
}

export function testPidPlatformArchitectureLookReasonable(): none {
  assert(pid() > 0, "expected pid to be positive")
  assert(platform().length > 0, "expected platform to be non-empty")
  assert(architecture().length > 0, "expected architecture to be non-empty")
}

export function testRunCapturesStdoutAndStderr(): none {
  let result: ExecResult | none = none
  defaultOptions := ExecOptions {}
  case run(
    "/bin/sh",
    ["-c", "printf 'out'; printf 'err' 1>&2"],
    defaultOptions
  ) {
    s: Success -> {
      result = s.value
    }
    f: Failure -> {
      assert(false, "expected run() to succeed: " + f.error)
    }
  }

  assert(result != none, "expected run() to produce a result")

  assert(result!.exitCode == 0, "expected shell command to exit successfully")
  assert(bytesToString(result!.stdout) == "out", "expected stdout to be captured")
  assert(bytesToString(result!.stderr) == "err", "expected stderr to be captured")
}

export function testRunBoundsCapturedOutputWhileDrainingToCompletion(): none {
  result := try! run(
    "/bin/sh",
    ["-c", "printf 'abcdef'; printf 'ghijkl' 1>&2"],
    ExecOptions { mergeStderrIntoStdout: true, maxOutputBytes: 5L },
  )

  assert(result.exitCode == 0, "expected bounded command to complete")
  assert(bytesToString(result.stdout) == "abcde", "expected capture to stop at the configured byte limit")
  assert(result.stdoutTruncated, "expected bounded stdout to report truncation")
  assert(result.stderr.length == 0, "expected merged stderr to leave the stderr capture empty")
  assert(!result.stderrTruncated, "expected the unused stderr capture not to report truncation")
}

class ConcurrentCommandRunner {
  command: string

  execute(): int {
    return (try! run("/bin/sh", ["-c", this.command], ExecOptions { withStdin: false })).exitCode
  }
}

export function testConcurrentRunsUseThreadSafeSpawning(): none {
  first := Actor<ConcurrentCommandRunner>("sleep 0.02")
  second := Actor<ConcurrentCommandRunner>("sleep 0.02")
  firstResult := async first.execute()
  secondResult := async second.execute()
  assert(try! firstResult.get() == 0, "expected first concurrent command to succeed")
  assert(try! secondResult.get() == 0, "expected second concurrent command to succeed")
  retire first
  retire second
}

function processGroupId(processId: int): string {
  result := try! run(
    "/bin/ps",
    ["-o", "pgid=", "-p", string(processId)],
    ExecOptions { withStdin: false },
  )
  assert(result.exitCode == 0, "expected ps to report the process group")
  return bytesToString(result.stdout).trim()
}

function childProcessGroupId(mode: ProcessGroupMode): string {
  result := try! run(
    "/bin/sh",
    ["-c", "ps -o pgid= -p $$"],
    ExecOptions { withStdin: false, processGroupMode: mode },
  )
  assert(result.exitCode == 0, "expected child shell to report its process group")
  return bytesToString(result.stdout).trim()
}

function defaultChildProcessGroupId(): string {
  result := try! run(
    "/bin/sh",
    ["-c", "ps -o pgid= -p $$"],
    ExecOptions { withStdin: false },
  )
  assert(result.exitCode == 0, "expected child shell to report its process group")
  return bytesToString(result.stdout).trim()
}

export function testProcessGroupModeControlsChildMembership(): none {
  parentGroup := processGroupId(pid())
  assert(defaultChildProcessGroupId() != parentGroup, "expected the default isolated child process group")
  assert(childProcessGroupId(.Isolated) != parentGroup, "expected the explicit isolated child process group")
  assert(childProcessGroupId(.Inherited) == parentGroup, "expected the child to inherit the caller process group")
}

export function testInheritedProcessGroupTimeoutDoesNotSignalCaller(): none {
  result := run(
    "/bin/sh",
    ["-c", "sleep 1"],
    ExecOptions {
      withStdin: false,
      processGroupMode: .Inherited,
      timeout: Duration.ofMillis(50L),
    },
  )
  case result {
    s: Success -> {
      assert(false, "expected inherited-group command to time out with exit code " + string(s.value.exitCode))
    }
    f: Failure -> {
      assert(f.error.contains("timed out"), "expected timeout failure without signalling the caller")
    }
  }
}

export function testRunAllowsCommandToCompleteBeforeTimeout(): none {
  let result: ExecResult | none = none
  options := ExecOptions {
    timeout: Duration.ofSeconds(1L)
  }

  case run(
    "/bin/sh",
    ["-c", "printf 'done'"],
    options
  ) {
    s: Success -> {
      result = s.value
    }
    f: Failure -> {
      assert(false, "expected run() to complete before timeout: " + f.error)
    }
  }

  assert(result != none, "expected run() to produce a result")
  assert(result!.exitCode == 0, "expected shell command to exit successfully")
  assert(bytesToString(result!.stdout) == "done", "expected stdout to be captured")
}

export function testRunTimesOutLongCommand(): none {
  options := ExecOptions {
    timeout: Duration.ofMillis(50L)
  }

  case run(
    "/bin/sh",
    ["-c", "sleep 1; printf 'late'"],
    options
  ) {
    s: Success -> {
      assert(false, "expected run() to fail on timeout with exit code " + string(s.value.exitCode))
    }
    f: Failure -> {
      assert(f.error.contains("timed out"), "expected timeout failure message")
    }
  }
}

export function testSpawnedProcessWaitUsesTimeout(): none {
  let proc: Exec | none = none
  options := ExecOptions {
    timeout: Duration.ofMillis(50L)
  }

  case Exec.spawn("/bin/sh", ["-c", "sleep 1"], options) {
    s: Success -> {
      proc = s.value
    }
    f: Failure -> {
      assert(false, "expected Exec.spawn to succeed: " + f.error)
    }
  }

  assert(proc != none, "expected Exec.spawn to produce a process")

  case proc!.wait() {
    s: Success -> {
      assert(false, "expected wait() to fail on timeout with exit code " + string(s.value))
    }
    f: Failure -> {
      assert(f.error.contains("timed out"), "expected timeout failure message")
    }
  }
}

export function testExecSupportsCwdAndEnvOverride(): none {
  options := ExecOptions {
    cwd: tempDirectory(),
    env: {
      "DOOF_OS_TEST": "present"
    }
  }

  let result: ExecResult | none = none
  case run(
    "/bin/sh",
    ["-c", "pwd; printf '%s' \"$DOOF_OS_TEST\""],
    options
  ) {
    s: Success -> {
      result = s.value
    }
    f: Failure -> {
      assert(false, "expected run() with cwd/env to succeed: " + f.error)
    }
  }

  assert(result != none, "expected run() to produce a result")

  output := bytesToString(result!.stdout)
  lines := output.split("\n")

  assert(lines.length >= 2, "expected cwd and env output lines")
  assert(lines[0].length > 0, "expected cwd line")
  assert(lines[lines.length - 1] == "present", "expected env override to be visible in child process")
}

export function testExecStdinPushAndStreaming(): none {
  let proc: Exec | none = none
  defaultOptions := ExecOptions {}
  case Exec.spawn("/bin/sh", ["-c", "cat"], defaultOptions) {
    s: Success -> {
      proc = s.value
    }
    f: Failure -> {
      assert(false, "expected Exec.spawn to succeed: " + f.error)
    }
  }

  assert(proc != none, "expected Exec.spawn to produce a process")

  assertSuccessVoid(proc!.writeStdinText("alpha\\n"), "expected first stdin write to succeed")
  assertSuccessVoid(proc!.writeStdinText("beta\\n"), "expected second stdin write to succeed")
  assertSuccessVoid(proc!.closeStdin(), "expected stdin close to succeed")

  stdoutBytes := collectStream(proc!.stdoutStream())
  stderrBytes := collectStream(proc!.stderrStream())

  let exitCode = -1
  case proc!.wait() {
    s: Success -> {
      exitCode = s.value
    }
    f: Failure -> {
      assert(false, "expected process wait to succeed: " + f.error)
    }
  }

  stdoutText := bytesToString(stdoutBytes).replaceAll("\r", "")
  assert(exitCode == 0, "expected cat to exit successfully")
  assert(stdoutText.contains("alpha"), "expected stdout to include the first stdin write")
  assert(stdoutText.contains("beta"), "expected stdout to include the second stdin write")
  assert(stderrBytes.length == 0, "expected no stderr from cat")
}
