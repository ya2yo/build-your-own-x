use std::{
    env,
    io::{Write, stdin, stdout},
    path::Path,
    process::{Child, ChildStdout, Command, Stdio},
};

/// syntax parse
fn parse_args(input: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut escaped = false;
    let mut in_single = false;
    let mut in_double = false;
    let mut token_start = false;// 用于区分空字符

    for ch in input.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
            token_start = true;
        } else if ch == '\\' && !in_single {
            // 转义字符
            escaped = true;
            token_start = true;
        } else if ch == '\'' && !in_double {
            // 单引号
            in_single = !in_single;
            token_start = true;
        } else if ch == '"' && !in_single {
            // 双引号
            in_double = !in_double;
            token_start = true;
        } else if ch.is_whitespace() && !in_double && !in_single {
            if token_start {
                tokens.push(std::mem::take(&mut current));
                token_start = false;
            }
        } else {
            current.push(ch);
            token_start = true;
        }
    }
    if token_start {
        tokens.push(current);
    }
    tokens
}

/// 管道切分
fn split_pipeline(input: &str) -> Vec<String> {
    let mut commands = Vec::new();
    let mut current = String::new();
    let mut in_single = false;
    let mut in_double = false;
    let mut escaped = false;

    for ch in input.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
        } else if ch == '\\' && !in_single {
            escaped = true;
            current.push(ch);
        } else if ch == '\'' && !in_double {
            in_single = !in_single;
            current.push(ch);
        } else if ch == '"' && !in_single {
            in_double = !in_double;
            current.push(ch);
        } else if ch == '|' && !in_single && !in_double {
            commands.push(std::mem::take(&mut current));
        } else {
            current.push(ch);
        }
    }

    if !current.trim().is_empty() {
        commands.push(current);
    }

    commands
}

fn main() {
    loop {
        let curr_dir = env::current_dir().unwrap();
        print!("{}> ", curr_dir.display());
        stdout().flush().unwrap();

        let mut input = String::new();
        match stdin().read_line(&mut input) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let input=input.trim();
        if input.is_empty() {
            continue;
        }

        let commands = split_pipeline(input);
        let mut prev_stdout: Option<ChildStdout> = None;
        let mut children: Vec<Child> = Vec::new();

        let cmd_count = commands.len();

        for (i, command) in commands.into_iter().enumerate() {
            let args = parse_args(&command);
            if args.is_empty() {
                continue;
            }

            let cmd = &args[0];
            let cmd_args = &args[1..];

            match cmd.as_str() {
                "cd" => {
                    // default to '/'
                    let new_dir = cmd_args.first().map_or("/", |x| (*x).as_str());
                    let root = Path::new(new_dir);
                    if let Err(e) = env::set_current_dir(root) {
                        eprintln!("{}", e);
                    }
                }
                "exit" => {
                    return;
                }
                _ => {
                    let stdin = match prev_stdout.take() {
                        Some(stdout_handle) => Stdio::from(stdout_handle),
                        None => Stdio::inherit(),
                    };

                    // 如果不是管道的最后一个命令，则输出重定向到 pipe
                    let is_last = i == cmd_count - 1;
                    let stdout = if is_last {
                        Stdio::inherit()
                    } else {
                        Stdio::piped()
                    };

                    match Command::new(cmd)
                        .args(cmd_args)
                        .stdin(stdin)
                        .stdout(stdout)
                        .spawn()
                    {
                        Ok(mut child) => {
                            if !is_last {
                                // 提取 stdout 供下一个子进程使用
                                prev_stdout = child.stdout.take();
                            }
                            children.push(child);
                        }
                        Err(e) => {
                            eprintln!("{}: {}", cmd, e);
                            break;
                        }
                    }
                }
            }
        }
        // 等待管道中的所有子进程完成，防止僵尸进程
        for mut child in children {
            let _ = child.wait();
        }
    }
}
