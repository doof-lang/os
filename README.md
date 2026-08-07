# std/os

`std/os` provides process and runtime OS helpers.

## Documentation

- [Guide and API reference](docs/API.md) explains environment helpers, process execution, streams, timeouts, and deadlock avoidance.
- Tests can be run with `doof test os`.

## API

- `env(name: string): Result<string, string>`
- `pid(): int`
- `platform(): string`
- `architecture(): string`
- `ProcessGroupMode` - selects isolated or inherited process-group membership.
- `Exec.spawn(command, args, options): Result<Exec, string>`
- `run(command, args, options): Result<ExecResult, string>`
- `run(command, args, ExecOptions { timeout: Duration.ofSeconds(5L) })`

## ExecOptions

- `cwd: string | null` - child process working directory.
- `env: Map<string, string>` - environment overrides for child process.
- `inheritEnv: bool` - whether to inherit parent environment before overrides.
- `withStdin: bool` - whether to open writable stdin for child process.
- `mergeStderrIntoStdout: bool` - redirect stderr into stdout to simplify single-stream reading.
- `inheritOutput: bool` - attach stdout and stderr to the parent instead of capturing them.
- `processGroupMode: ProcessGroupMode` - use an isolated process group by default, or inherit the caller's group for terminal-attached interactive children.
- `maxOutputBytes: long | null` - retain at most this many bytes per captured stream while continuing to drain the child.
- `timeout: Duration | null` - optional process timeout, measured from `Exec.spawn()`. `run()` and `wait()` terminate the process and return a failure when the timeout is reached.

## Exec

- `stdoutStream(): Stream<readonly byte[]>`
- `stderrStream(): Stream<readonly byte[]>`
- `writeStdinText(value: string): Result<void, string>`
- `closeStdin(): Result<void, string>`
- `isRunning(): bool`
- `wait(): Result<int, string>`
- `terminate(signal: int = 15): Result<void, string>`

## Notes

- `stdoutStream()` and `stderrStream()` are blocking pull streams and return `null` on EOF.
- `timeout` is enforced by `run()` and `wait()`; individual blocking stream reads do not wake up on the timeout by themselves.
- If your child process writes heavily to stderr while you only consume stdout, use `mergeStderrIntoStdout: true` to avoid pipe backpressure deadlocks.
- `ExecResult.stdoutTruncated` and `stderrTruncated` report when bounded capture discarded bytes.
- POSIX child creation uses `posix_spawn`, so concurrent callers do not fork a multithreaded process.
- `ProcessGroupMode.Inherited` controls process-group membership only; it does not implement shell-style background execution or terminal job control.
