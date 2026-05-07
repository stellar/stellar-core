#!/usr/bin/env python3

"""Orchestrates apply-load runs on a fixed-spec AWS instance from an arbitrary 
   caller.

This runs on both the control machine (e.g. Jenkins agent) and the remote 
AWS instance.

The usage modes are as follows:

* (control machine) launches and prepares a fixed-spec AWS instance with ``aws-init``
* (remote AWS instance) bootstraps itself with ``local-aws-init``
* (control machine) invokes one of the apply-load modes remotely with ``aws-run <apply-load-mode>``
* (remote AWS instance) runs apply-load with ``<apply-load-mode>``

All the available apply-load modes are supported. They are customized with 
apply-load-aws-*.cfg templates in this directory, where every template argument
is populated from the CLI parameters of ``<apply-load-mode>`` command.
"""

import argparse
import base64
import binascii
import json
import shlex
import string
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Mapping, Optional, Sequence

# Instance type to use. Matches SDF validator instance type.
INSTANCE_TYPE = "c5d.2xlarge"

# Directory containing helper files for this script.
APPLY_LOAD_SCRIPT_DIR = Path(__file__).resolve().parent

# User directory on the instance.
USER_DIR = "/home/ubuntu"
REMOTE_APPLY_LOAD_DIR = f"{USER_DIR}/apply_load"
REMOTE_SCRIPT_PATH = f"{REMOTE_APPLY_LOAD_DIR}/apply_load_aws.py"
APPLY_LOAD_LOG_FILE_PATH = "/tmp/apply-load.log"
APPLY_LOAD_LOG_DIR = str(PurePosixPath(APPLY_LOAD_LOG_FILE_PATH).parent)

# Path to the ephemeral NVMe drive on AWS instance.
NVME_DRIVE = "/dev/nvme1n1"

# Number of SSM connection retries before giving up.
SSM_RETRIES = 10

# Maximum time to wait for an SSM command to complete.
SSM_COMMAND_TIMEOUT_SECONDS = 85 * 60

# Delay between SSM command status polls.
SSM_COMMAND_POLL_INTERVAL_SECONDS = 5

# Chunk size used when downloading a remote file over SSM stdout.
REMOTE_FILE_CHUNK_SIZE_BYTES = 12 * 1024

# Preserve the legacy tag value required by the existing EC2 IAM policy.
INSTANCE_TEST_TAG_VALUE = "max-sac-tps"


@dataclass(frozen=True)
class ParameterDefinition:
    """CLI metadata for one template-backed parameter."""

    arg_type: type
    help: str
    choices: Optional[Sequence[str]] = None


MODE_CONFIGS = {
    "benchmark": {
        "template": APPLY_LOAD_SCRIPT_DIR / "apply-load-aws-benchmark.cfg",
        "help": "Run apply-load in benchmark mode.",
        "aliases": [],
    },
    "ledger-limits": {
        "template": APPLY_LOAD_SCRIPT_DIR / "apply-load-aws-ledger-limits.cfg",
        "help": "Run apply-load in ledger-limits mode.",
        "aliases": [],
    },
    "max-sac-tps": {
        "template": APPLY_LOAD_SCRIPT_DIR / "apply-load-aws-max-sac-tps.cfg",
        "help": "Run apply-load in max-sac-tps mode.",
        "aliases": ["max-sac"],
    },
}


FIXED_TEMPLATE_VALUES = {
    "log_file_path": APPLY_LOAD_LOG_FILE_PATH,
}


PARAMETER_DEFINITIONS = {
    "dependent_tx_clusters": ParameterDefinition(
        int,
        "Number of dependent transaction clusters.",
    ),
    "ledger_max_disk_read_bytes": ParameterDefinition(
        int,
        "Maximum disk read bytes for the ledger.",
    ),
    "ledger_max_disk_read_ledger_entries": ParameterDefinition(
        int,
        "Maximum disk read ledger entries for the ledger.",
    ),
    "ledger_max_instructions": ParameterDefinition(
        int,
        "Maximum instructions allowed for the ledger.",
    ),
    "ledger_max_write_bytes": ParameterDefinition(
        int,
        "Maximum ledger write bytes.",
    ),
    "ledger_max_write_ledger_entries": ParameterDefinition(
        int,
        "Maximum ledger write ledger entries.",
    ),
    "max_contract_event_size_bytes": ParameterDefinition(
        int,
        "Maximum contract event size in bytes.",
    ),
    "max_ledger_tx_size_bytes": ParameterDefinition(
        int,
        "Maximum total transaction size per ledger in bytes.",
    ),
    "max_tps": ParameterDefinition(
        int,
        "Upper TPS bound for max-sac-tps binary search.",
    ),
    "max_tx_size_bytes": ParameterDefinition(
        int,
        "Maximum transaction size in bytes.",
    ),
    "min_tps": ParameterDefinition(
        int,
        "Lower TPS bound for max-sac-tps binary search.",
    ),
    "model_tx": ParameterDefinition(
        str,
        "Benchmark model transaction kind.",
        choices=("sac", "custom_token", "soroswap"),
    ),
    "num_events": ParameterDefinition(
        int,
        "Number of contract events emitted by each transaction.",
    ),
    "num_ledgers": ParameterDefinition(
        int,
        "Number of ledgers to close.",
    ),
    "target_close_time_ms": ParameterDefinition(
        int,
        "Target close time in milliseconds for max-sac-tps mode.",
    ),
    "tx_count": ParameterDefinition(
        int,
        "Number of Soroban transactions to apply per ledger.",
    ),
    "tx_max_disk_read_bytes": ParameterDefinition(
        int,
        "Maximum per-transaction disk read bytes.",
    ),
    "tx_max_disk_read_ledger_entries": ParameterDefinition(
        int,
        "Maximum per-transaction disk read ledger entries.",
    ),
    "tx_max_footprint_size": ParameterDefinition(
        int,
        "Maximum per-transaction footprint size.",
    ),
    "tx_max_instructions": ParameterDefinition(
        int,
        "Maximum instructions allowed per transaction.",
    ),
    "tx_max_write_bytes": ParameterDefinition(
        int,
        "Maximum per-transaction write bytes.",
    ),
    "tx_max_write_ledger_entries": ParameterDefinition(
        int,
        "Maximum per-transaction write ledger entries.",
    ),
}


def format_command(command: Sequence[Any]) -> str:
    return subprocess.list2cmdline([str(part) for part in command])


def run(command: Sequence[Any], capture_output: bool = False,
        check: bool = True) -> subprocess.CompletedProcess[str]:
    command_args = [str(part) for part in command]
    printable_command = format_command(command_args)
    print(f"Running: {printable_command}")
    if capture_output:
        result = subprocess.run(
            command_args,
            capture_output=True,
            check=False,
            text=True,
        )
    else:
        result = subprocess.run(
            command_args,
            check=False,
            text=True,
        )
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.stderr:
            print(result.stderr, end="" if result.stderr.endswith("\n") else "\n")
        if check:
            raise SystemExit(
                f"Command '{printable_command}' failed with exit code "
                f"{result.returncode}"
            )
    return result


def run_capture_output(command: Sequence[Any], check: bool = True) -> str:
    return run(command, capture_output=True, check=check).stdout.strip()


def get_template_parameters(template_path: Path) -> list[str]:
    formatter = string.Formatter()
    parameters = []
    seen = set()
    template = template_path.read_text(encoding="utf-8")
    for _, field_name, _, _ in formatter.parse(template):
        if field_name and field_name not in seen:
            seen.add(field_name)
            parameters.append(field_name)
    return parameters


def get_mode_parameters(mode_name: str) -> list[str]:
    """Return all template parameters referenced by a mode's config file."""

    template_path = MODE_CONFIGS[mode_name]["template"]
    parameters = get_template_parameters(template_path)
    undefined_parameters = [
        parameter for parameter in parameters
        if parameter not in PARAMETER_DEFINITIONS
        and parameter not in FIXED_TEMPLATE_VALUES
    ]
    if undefined_parameters:
        joined = ", ".join(undefined_parameters)
        raise RuntimeError(
            f"Unsupported template parameter(s) for mode '{mode_name}': "
            f"{joined}"
        )
    return parameters


def get_mode_cli_parameters(mode_name: str) -> list[str]:
    return [
        parameter for parameter in get_mode_parameters(mode_name)
        if parameter not in FIXED_TEMPLATE_VALUES
    ]


def render_config(mode_name: str, values: Mapping[str, Any]) -> str:
    """Render the selected apply-load config template from CLI values."""

    template_path = MODE_CONFIGS[mode_name]["template"]
    parameter_names = get_mode_parameters(mode_name)
    template_values = {
        parameter: FIXED_TEMPLATE_VALUES[parameter]
        for parameter in parameter_names
        if parameter in FIXED_TEMPLATE_VALUES
    }
    missing_parameters = [
        parameter for parameter in parameter_names
        if parameter not in template_values and values.get(parameter) is None
    ]
    if missing_parameters:
        joined = ", ".join(sorted(missing_parameters))
        raise ValueError(
            f"Missing required template parameter(s) for mode '{mode_name}': "
            f"{joined}"
        )

    template_values.update({
        parameter: values[parameter]
        for parameter in parameter_names
        if parameter not in FIXED_TEMPLATE_VALUES
    })
    template = template_path.read_text(encoding="utf-8")
    return template.format(**template_values)


def add_template_arguments(parser: argparse.ArgumentParser,
                           mode_name: str) -> None:
    for parameter in get_mode_cli_parameters(mode_name):
        definition = PARAMETER_DEFINITIONS[parameter]
        argument_name = f"--{parameter.replace('_', '-')}"
        kwargs = {
            "dest": parameter,
            "help": definition.help,
            "required": True,
            "type": definition.arg_type,
        }
        if definition.choices is not None:
            kwargs["choices"] = definition.choices
        parser.add_argument(argument_name, **kwargs)


def add_run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--image",
        required=True,
        help="Docker image to use.",
    )
    parser.add_argument(
        "--iops",
        type=int,
        help="Optional disk IOPS limit for the NVMe device.",
    )


def add_aws_run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--instance-id",
        required=True,
        help="EC2 instance id to run apply-load on.",
    )
    parser.add_argument(
        "--region",
        required=True,
        help="AWS region of the instance.",
    )
    parser.add_argument(
        "--local-log-path",
        required=True,
        help="Local file path to store the downloaded apply-load log.",
    )
    parser.add_argument(
        "--s3-bucket",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--s3-log-key",
        help=argparse.SUPPRESS,
    )


def build_docker_command(config_path: str, image: str,
                         iops: Optional[int]) -> list[str]:
    command = [
        "docker",
        "run",
        "--init",
        "--rm",
        "-v",
        f"{config_path}:/config.cfg",
        "-v",
        f"{APPLY_LOAD_LOG_DIR}:{APPLY_LOAD_LOG_DIR}",
    ]
    if iops is not None:
        command.extend([
            "--device-write-iops",
            f"{NVME_DRIVE}:{iops}",
            "--device-read-iops",
            f"{NVME_DRIVE}:{iops}",
        ])
    command.extend([
        image,
        "apply-load",
        "--console",
        "--conf",
        "/config.cfg",
    ])
    return command


def read_file_text(path: Path) -> Optional[str]:
    if not path.exists():
        return None

    return path.read_text(encoding="utf-8", errors="replace")


def build_apply_load_command(mode_name: str, values: Mapping[str, Any],
                             image: str, iops: Optional[int]) -> list[str]:
    """Build the helper command line for a specific local apply-load mode."""

    command = [
        "python3",
        "apply_load_aws.py",
        mode_name,
        "--image",
        image,
    ]
    if iops is not None:
        command.extend(["--iops", str(iops)])
    for parameter in get_mode_cli_parameters(mode_name):
        command.extend([
            f"--{parameter.replace('_', '-')}",
            str(values[parameter]),
        ])
    return command


def build_remote_apply_load_command(mode_name: str, values: Mapping[str, Any],
                                    image: str,
                                    iops: Optional[int]) -> str:
    """Wrap a local mode invocation so it can run under ``sudo -u ubuntu``."""

    apply_load_command = " ".join(
        shlex.quote(str(part))
        for part in build_apply_load_command(mode_name, values, image, iops)
    )
    shell_command = " ".join([
        # Clear the previous run's log before starting a new remote invocation.
        "rm -f",
        shlex.quote(APPLY_LOAD_LOG_FILE_PATH),
        "&& cd",
        shlex.quote(REMOTE_APPLY_LOAD_DIR),
        "&&",
        apply_load_command,
    ])
    return "sudo -u ubuntu bash -lc " + shlex.quote(shell_command)


def start_ec2_instance(ami: str, region: str, security_group: str,
                       iam_instance_profile: str) -> str:
    """Start an EC2 instance and return its instance id."""
    print("Starting EC2 instance...")
    command = [
        "aws",
        "ec2",
        "run-instances",
        "--image-id",
        ami,
        "--instance-type",
        INSTANCE_TYPE,
        "--security-groups",
        security_group,
        "--iam-instance-profile",
        f"Name={iam_instance_profile}",
        "--tag-specifications",
        f"ResourceType=instance,Tags=[{{Key=test,Value={INSTANCE_TEST_TAG_VALUE}}},"
        "{Key=ManagedBy,Value=ApplyLoadScript}]",
        "--query",
        "Instances[0].InstanceId",
        "--output",
        "text",
        "--region",
        region,
    ]
    instance_id = run_capture_output(command)
    print("Started EC2 instance with ID:", instance_id)

    print("Waiting for instance to be in 'running' state...")
    run([
        "aws",
        "ec2",
        "wait",
        "instance-running",
        "--instance-ids",
        instance_id,
        "--region",
        region,
    ])
    return instance_id


def wait_for_ssm_agent(instance_id: str, region: str) -> None:
    """Wait for SSM agent to be ready on the instance."""
    print("Waiting for SSM agent to be ready...")
    for attempt in range(SSM_RETRIES):
        command = [
            "aws",
            "ssm",
            "describe-instance-information",
            "--instance-information-filter-list",
            f"key=InstanceIds,valueSet={instance_id}",
            "--region",
            region,
        ]
        output = run_capture_output(command, check=False)
        if output:
            info = json.loads(output)
            if info.get("InstanceInformationList"):
                print("SSM agent is ready.")
                return

        sleep_duration = 10
        print(
            f"SSM agent not ready yet, retrying "
            f"({attempt + 1}/{SSM_RETRIES}) in {sleep_duration} seconds..."
        )
        time.sleep(sleep_duration)

    raise SystemExit("ERROR: SSM agent failed to become ready")


def run_ssm_command_result(instance_id: str, region: str, command: str,
                           log_output: bool = True,
                           check: bool = True) -> tuple[str, str]:
    """Run a command on an EC2 instance via SSM and return stdout/stderr."""
    print(f"Running SSM command on {instance_id}: {command}")

    send_command = [
        "aws",
        "ssm",
        "send-command",
        "--instance-ids",
        instance_id,
        "--document-name",
        "AWS-RunShellScript",
        "--parameters",
        json.dumps({"commands": [command]}),
        "--region",
        region,
        "--query",
        "Command.CommandId",
        "--output",
        "text",
    ]
    command_id = run_capture_output(send_command)

    print(f"Waiting for command {command_id} to complete...")
    terminal_statuses = {"Success", "Failed", "Cancelled", "TimedOut"}
    total_polls = max(
        1,
        (
            SSM_COMMAND_TIMEOUT_SECONDS + SSM_COMMAND_POLL_INTERVAL_SECONDS - 1
        ) // SSM_COMMAND_POLL_INTERVAL_SECONDS,
    )
    for attempt in range(total_polls):
        status_command = [
            "aws",
            "ssm",
            "get-command-invocation",
            "--command-id",
            command_id,
            "--instance-id",
            instance_id,
            "--region",
            region,
            "--query",
            "Status",
            "--output",
            "text",
        ]
        status_result = run(status_command, capture_output=True, check=False)
        if status_result.returncode != 0 or not status_result.stdout:
            time.sleep(SSM_COMMAND_POLL_INTERVAL_SECONDS)
            continue

        status = status_result.stdout.strip()
        print(
            f"Command status: {status} "
            f"({attempt + 1}/{total_polls})"
        )
        if status not in terminal_statuses:
            time.sleep(SSM_COMMAND_POLL_INTERVAL_SECONDS)
            continue

        # Once the command is terminal, fetch the final stdout/stderr payload.
        invocation_command = [
            "aws",
            "ssm",
            "get-command-invocation",
            "--command-id",
            command_id,
            "--instance-id",
            instance_id,
            "--region",
            region,
        ]
        invocation_result = run(
            invocation_command, capture_output=True, check=False
        )
        if invocation_result.returncode != 0 or not invocation_result.stdout:
            raise SystemExit(
                f"Failed to fetch final SSM invocation output for '{command}'"
            )

        result = json.loads(invocation_result.stdout)

        stdout = result.get("StandardOutputContent", "").strip()
        stderr = result.get("StandardErrorContent", "").strip()
        if log_output and stdout:
            print(stdout, end="" if stdout.endswith("\n") else "\n")
        if log_output and stderr:
            print(stderr, end="" if stderr.endswith("\n") else "\n")

        if check and status != "Success":
            raise SystemExit(
                f"SSM command '{command}' failed with status {status}"
            )
        return stdout, stderr

    raise SystemExit(
        f"ERROR: Command '{command}' timed out after "
        f"{SSM_COMMAND_TIMEOUT_SECONDS} seconds"
    )


def run_ssm_command(instance_id: str, region: str, command: str) -> None:
    """Run a command on an EC2 instance via SSM."""
    run_ssm_command_result(instance_id, region, command)


def copy_file_via_s3(instance_id: str, region: str, local_file: Path,
                    remote_path: str, s3_bucket: str) -> None:
    """Copy a file to an EC2 instance via S3 and SSM."""
    s3_key = f"tmp/{local_file.name}"
    s3_uri = f"s3://{s3_bucket}/{s3_key}"
    print(f"Uploading {local_file} to {s3_uri}...")
    try:
        run([
            "aws",
            "s3",
            "cp",
            str(local_file),
            s3_uri,
            "--region",
            region,
        ])
        print(f"Downloading file to instance at {remote_path}...")
        run_ssm_command(
            instance_id,
            region,
            f"aws s3 cp {s3_uri} {remote_path} --region {region}",
        )
    finally:
        run([
            "aws",
            "s3",
            "rm",
            s3_uri,
            "--region",
            region,
        ], check=False)


def copy_files_to_instance(instance_id: str, region: str,
                           s3_bucket: str) -> None:
    """Copy files to the instance using SSM and S3."""
    print("Copying files to instance via S3...")
    run_ssm_command(instance_id, region, f"mkdir -p {REMOTE_APPLY_LOAD_DIR}")

    copy_file_via_s3(
        instance_id,
        region,
        APPLY_LOAD_SCRIPT_DIR / "apply_load_aws.py",
        REMOTE_SCRIPT_PATH,
        s3_bucket,
    )
    run_ssm_command(instance_id, region, f"chmod +x {REMOTE_SCRIPT_PATH}")

    for mode_config in MODE_CONFIGS.values():
        template_path = mode_config["template"]
        copy_file_via_s3(
            instance_id,
            region,
            template_path,
            f"{REMOTE_APPLY_LOAD_DIR}/{template_path.name}",
            s3_bucket,
        )


def install_script_on_instance(instance_id: str, region: str,
                               s3_bucket: str) -> str:
    """Install this script on the given EC2 instance via SSM."""
    wait_for_ssm_agent(instance_id, region)
    install_awscli(instance_id, region)
    copy_files_to_instance(instance_id, region, s3_bucket)
    return instance_id


def install_awscli(instance_id: str, region: str) -> None:
    """Install AWS CLI on Ubuntu Linux machine."""
    print("Installing AWS CLI...")
    install_command = " ".join([
        "set -eu;",
        "apt-get update;",
        "apt-get install -y unzip curl;",
        "curl \"https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip\" -o awscliv2.zip;",
        "unzip -q awscliv2.zip;",
        "./aws/install;",
        "rm -rf aws awscliv2.zip;",
    ])
    run_ssm_command(instance_id, region, install_command)
    print("AWS CLI installation complete.")


def local_aws_init() -> None:
    """Initialize a local AWS instance for running apply-load."""
    run(["sudo", "mkfs.ext4", NVME_DRIVE])
    run(["sudo", "mkdir", "-p", "/var/lib/docker"])
    run(["sudo", "mount", NVME_DRIVE, "/var/lib/docker"])
    run(["sudo", "apt-get", "update"])
    run(["sudo", "apt-get", "install", "-y", "docker.io"])
    run(["sudo", "usermod", "-aG", "docker", "ubuntu"])


def aws_init(ami: str, region: str, security_group: str,
             iam_instance_profile: str, s3_bucket: str) -> None:
    """Create and initialize an AWS instance for running apply-load."""
    instance_id = start_ec2_instance(
        ami, region, security_group, iam_instance_profile
    )
    install_script_on_instance(instance_id, region, s3_bucket)
    run_ssm_command(
        instance_id,
        region,
        f"cd {REMOTE_APPLY_LOAD_DIR} && python3 apply_load_aws.py local-aws-init",
    )
    print(instance_id)


def run_apply_load(config: str, image: str, iops: Optional[int]) -> None:
    """Run apply-load with the given configuration."""
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", delete=False
    ) as config_file:
        config_file.write(config)
        config_path = config_file.name

    log_path = Path(APPLY_LOAD_LOG_FILE_PATH)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.unlink(missing_ok=True)

    try:
        result = run(
            build_docker_command(config_path, image, iops),
            capture_output=True,
            check=False,
        )
        if read_file_text(log_path) is None:
            print(f"WARNING: apply-load core log not found at {log_path}")
        if result.returncode != 0:
            raise SystemExit(
                f"apply-load failed with exit code {result.returncode}; "
                f"see {log_path}"
            )
    finally:
        Path(config_path).unlink(missing_ok=True)


def download_apply_load_log(instance_id: str, region: str,
                            local_path: Path) -> None:
    """Download the remote apply-load log over SSM in base64-encoded chunks.

    We don't use s3 here because the role on the other instance doesn't 
    necessarily have s3 write permissions. Since logs are rather small, this
    simplifies the pipeline setup.
    """

    remote_path = APPLY_LOAD_LOG_FILE_PATH

    size_command = "sudo -u ubuntu bash -lc " + shlex.quote(
        f"if [ -f {shlex.quote(remote_path)} ]; then wc -c < {shlex.quote(remote_path)}; else echo MISSING; fi"
    )
    stdout, _ = run_ssm_command_result(
        instance_id,
        region,
        size_command,
        log_output=False,
    )
    size_text = stdout.strip()
    if not size_text or size_text == "MISSING":
        print(f"WARNING: apply-load log not found at {remote_path}")
        return

    file_size = int(size_text)
    local_path.parent.mkdir(parents=True, exist_ok=True)
    file_descriptor, temp_name = tempfile.mkstemp(dir=local_path.parent)
    temp_path = Path(temp_name)
    try:
        # Write into a temp file so Jenkins never archives a partial log.
        with open(file_descriptor, "wb", closefd=True) as output_file:
            chunk_count = (
                file_size + REMOTE_FILE_CHUNK_SIZE_BYTES - 1
            ) // REMOTE_FILE_CHUNK_SIZE_BYTES
            for chunk_index in range(chunk_count):
                chunk_command = "sudo -u ubuntu bash -lc " + shlex.quote(
                    " ".join([
                        "dd",
                        f"if={shlex.quote(remote_path)}",
                        f"bs={REMOTE_FILE_CHUNK_SIZE_BYTES}",
                        f"skip={chunk_index}",
                        "count=1",
                        "status=none",
                        "|",
                        "base64",
                        "-w",
                        "0",
                    ])
                )
                chunk_stdout, _ = run_ssm_command_result(
                    instance_id,
                    region,
                    chunk_command,
                    log_output=False,
                )
                encoded_chunk = "".join(chunk_stdout.split())
                try:
                    decoded_chunk = base64.b64decode(
                        encoded_chunk,
                        validate=True,
                    )
                except binascii.Error as exc:
                    raise RuntimeError(
                        f"Failed to decode base64 chunk {chunk_index} from "
                        f"{remote_path}"
                    ) from exc
                output_file.write(decoded_chunk)
        temp_path.replace(local_path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise


def run_apply_load_on_instance(instance_id: str, region: str,
                               local_log_path: Path, mode_name: str,
                               values: Mapping[str, Any], image: str,
                               iops: Optional[int]) -> None:
    """Run one apply-load mode remotely and always fetch its log afterward."""

    run_error = None
    remote_command = build_remote_apply_load_command(
        mode_name, values, image, iops
    )
    try:
        run_ssm_command(instance_id, region, remote_command)
    except SystemExit as exc:
        run_error = exc

    download_apply_load_log(instance_id, region, local_log_path)

    if run_error is not None:
        raise run_error


def handle_aws_init(args: argparse.Namespace) -> None:
    aws_init(
        args.ubuntu_ami,
        args.region,
        args.security_group,
        args.iam_instance_profile,
        args.s3_bucket,
    )


def handle_local_aws_init(_: argparse.Namespace) -> None:
    local_aws_init()


def handle_run_mode(args: argparse.Namespace) -> None:
    config = render_config(args.apply_load_mode, vars(args))
    run_apply_load(config, args.image, args.iops)


def handle_aws_run_mode(args: argparse.Namespace) -> None:
    run_apply_load_on_instance(
        args.instance_id,
        args.region,
        Path(args.local_log_path),
        args.apply_load_mode,
        vars(args),
        args.image,
        args.iops,
    )


def build_parser() -> argparse.ArgumentParser:
    """Build the CLI with matching local and remote entrypoints."""

    parser = argparse.ArgumentParser(
        description="Helper script to run apply-load tests on AWS"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    aws_init_parser = subparsers.add_parser(
        "aws-init",
        help="Create and initialize an AWS instance for running apply-load.",
    )
    aws_init_parser.add_argument(
        "--ubuntu-ami",
        required=True,
        help="AMI ID to use. Must be an Ubuntu image.",
    )
    aws_init_parser.add_argument(
        "--region",
        required=True,
        help="AWS region to use.",
    )
    aws_init_parser.add_argument(
        "--security-group",
        required=True,
        help="AWS security group to use.",
    )
    aws_init_parser.add_argument(
        "--iam-instance-profile",
        required=True,
        help="IAM instance profile to use.",
    )
    aws_init_parser.add_argument(
        "--s3-bucket",
        required=True,
        help="S3 bucket to use for file transfer.",
    )
    aws_init_parser.set_defaults(handler=handle_aws_init)

    local_aws_init_parser = subparsers.add_parser(
        "local-aws-init",
        help="Initialize the local AWS instance for running apply-load.",
    )
    local_aws_init_parser.set_defaults(handler=handle_local_aws_init)

    aws_run_parser = subparsers.add_parser(
        "aws-run",
        help="Run apply-load on an existing AWS instance.",
    )
    aws_run_subparsers = aws_run_parser.add_subparsers(
        dest="apply_load_mode", required=True
    )
    # Expose each mode under aws-run so Jenkins can reuse the local schema.
    for mode_name, mode_config in MODE_CONFIGS.items():
        mode_parser = aws_run_subparsers.add_parser(
            mode_name,
            aliases=list(mode_config["aliases"]),
            help=mode_config["help"],
        )
        add_aws_run_arguments(mode_parser)
        add_run_arguments(mode_parser)
        add_template_arguments(mode_parser, mode_name)
        mode_parser.set_defaults(
            handler=handle_aws_run_mode,
            apply_load_mode=mode_name,
        )

    # Expose the same modes locally for development and ad-hoc debugging.
    for mode_name, mode_config in MODE_CONFIGS.items():
        mode_parser = subparsers.add_parser(
            mode_name,
            aliases=list(mode_config["aliases"]),
            help=mode_config["help"],
        )
        add_run_arguments(mode_parser)
        add_template_arguments(mode_parser, mode_name)
        mode_parser.set_defaults(
            handler=handle_run_mode,
            apply_load_mode=mode_name,
        )

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    try:
        args.handler(args)
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
