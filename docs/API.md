# std/os Guide

`std/os` exposes process and runtime operating-system helpers. Use the simple
helpers for environment and platform discovery, `run` for command execution when
you want all output buffered, and `Exec.spawn` when you need streaming output or
interactive stdin.

## Quick Start

```doof
import { ExecOptions, run } from "std/os"
import { Duration } from "std/time"

result := try! run(
  "git",
  ["status", "--short"],
  ExecOptions {
    timeout: Duration.ofSeconds(5L),
    mergeStderrIntoStdout: true,
  },
)

println("exit: ${result.exitCode}")
```

## Environment And Platform

`env(name)` reads one environment variable and returns `Failure` when it is not
available. `pid()`, `platform()`, and `architecture()` query the current process
and runtime platform.

`platform()` returns `darwin`, `linux`, or `windows` on supported hosts.
`architecture()` reports `x64`, `x86`, `arm64`, or `arm`, and additionally
recognizes Linux `riscv64`, `ppc64`, `ppc64le`, `s390x`, and `loong64` hosts.

## Running Commands

Use `run(command, args, options)` for the common case. It spawns the child,
waits for completion, buffers stdout and stderr, and returns `ExecResult`.
Non-zero exit codes are represented in `ExecResult.exitCode`; spawn failures,
I/O failures, and timeouts return `Failure<string>`.

Use `Exec.spawn(command, args, options)` when the caller must stream output,
write stdin, terminate the process, or wait later.

## Streams, Stdin, And Deadlocks

`Exec.stdoutStream()` and `Exec.stderrStream()` are blocking pull streams. If a
child writes a lot to stderr while the caller only reads stdout, the child can
block on a full stderr pipe. Set `mergeStderrIntoStdout: true` for single-stream
consumption, or read both streams.

`withStdin` defaults to `true`, so spawned children get a writable stdin pipe.
Call `closeStdin()` after writing all input so programs waiting for EOF can
finish.

## Timeouts

`ExecOptions.timeout` is measured from process spawn. `run()` and `wait()`
terminate the process and return `Failure` when the timeout is reached.
Individual blocking stream reads do not wake themselves up at the timeout; use
`run()` or coordinate stream reads carefully if timeout behavior matters.

## API

### Runtime helpers

```doof
export function env(name: string): Result<string, string>
export function pid(): int
export function platform(): string
export function architecture(): string
```

Defined in [index.do](../index.do).

### `ProcessGroupMode`

```doof
export enum ProcessGroupMode {
  Isolated,
  Inherited,
}
```

### `ExecOptions`

```doof
export class ExecOptions
```

Fields:

- `cwd: string | null = null` sets the child working directory.
- `env: Map<string, string> = {}` adds or overrides child environment values.
- `inheritEnv: bool = true` starts from the parent environment before applying `env`.
- `withStdin: bool = true` opens a writable stdin pipe.
- `mergeStderrIntoStdout: bool = false` redirects stderr into stdout.
- `inheritOutput: bool = false` attaches child output directly to the parent process.
- `processGroupMode: ProcessGroupMode = .Isolated` creates a separate process group by default. Use `.Inherited` for terminal-attached interactive children that must remain in the caller's foreground process group.
- `maxOutputBytes: long | null = null` bounds retained bytes per captured stream while still draining all child output.
- `timeout: Duration | null = null` sets an optional process timeout.

Defined in [index.do](../index.do).

`ProcessGroupMode` controls process-group membership, independently of stdin and
output routing. `.Isolated` supports process-group cleanup on timeout, while
`.Inherited` causes timeout cleanup to signal only the child process. It does
not provide shell-style background execution or terminal job control.

### `Exec`

```doof
export class Exec
```

Methods:

- `static spawn(command: string, args: string[] = [], options: ExecOptions = ExecOptions {}): Result<Exec, string>`
- `stdoutStream(): Stream<readonly byte[]>`
- `stderrStream(): Stream<readonly byte[]>`
- `nextStdoutChunk(): readonly byte[] | null`
- `nextStderrChunk(): readonly byte[] | null`
- `writeStdinText(value: string): Result<void, string>`
- `closeStdin(): Result<void, string>`
- `isRunning(): bool`
- `wait(): Result<int, string>`
- `terminate(signal: int = 15): Result<void, string>`
- `stdoutOpen(): bool`
- `stderrOpen(): bool`

Defined in [index.do](../index.do).

### `ExecResult`

```doof
export class ExecResult
```

Fields:

- `exitCode: int`
- `stdout: readonly byte[]`
- `stderr: readonly byte[]`
- `stdoutTruncated: bool`
- `stderrTruncated: bool`

Defined in [index.do](../index.do).

### `run`

```doof
export function run(command: string, args: string[] = [], options: ExecOptions = ExecOptions {}): Result<ExecResult, string>
```

Run a child process to completion and collect stdout and stderr. When
`maxOutputBytes` is set, capture stops growing at the limit but the pipes keep
draining so the child cannot deadlock. Use `inheritOutput` when output should
remain attached to the invoking terminal.

Defined in [index.do](../index.do).
