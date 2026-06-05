# std/os

`std/os` provides process and runtime OS helpers.

## API

- `env(name: string): Result<string, string>`
- `pid(): int`
- `platform(): string`
- `architecture(): string`
- `Exec.spawn(command, args, options): Result<Exec, string>`
- `run(command, args, options): Result<ExecResult, string>`
- `run(command, args, ExecOptions { timeout: Duration.ofSeconds(5L) })`

## ExecOptions

- `cwd: string | null` - child process working directory.
- `env: Map<string, string>` - environment overrides for child process.
- `inheritEnv: bool` - whether to inherit parent environment before overrides.
- `withStdin: bool` - whether to open writable stdin for child process.
- `mergeStderrIntoStdout: bool` - redirect stderr into stdout to simplify single-stream reading.
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
