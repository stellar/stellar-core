#!/usr/bin/env python3

import json
import os
import subprocess
import tempfile
import time
from datetime import datetime

# Instance type to use. Matches SDF validator instance type.
INSTANCE_TYPE = 'c5d.2xlarge'

# Directory containing helper files for this script
APPLY_LOAD_SCRIPT_DIR = os.path.join(os.path.dirname(__file__), "apply_load")

# Path to the max SAC template configuration file
MAX_SAC_TEMPLATE = os.path.join(APPLY_LOAD_SCRIPT_DIR, "max-sac-template.cfg")

# User directory on the instance
USER_DIR = "/home/ssm-user"

# Path to the ephemeral NVMe drive on AWS instance
NVME_DRIVE = "/dev/nvme1n1"

# Number of SSH connection retries before giving up
SSH_RETRIES = 10

def run(command, exit_on_fail=True):
    """ Run a command and exit if it fails. Prints the command's output. """
    print(f"Running: {command}")
    res = os.system(command)
    if res != 0:
        print(f"Command '{command}' failed with exit code {res}")
        if exit_on_fail:
            exit(1)
        return False
    return True

def run_capture_output(command):
    """ Run a command and exit if it fails. Returns the command's output. """
    try:
        return subprocess.check_output(command)
    except subprocess.CalledProcessError as e:
        print(f"Command '{command}' failed with exit code {e.returncode}")
        exit(1)

# TODO: If anything fails AFTER starting the instance, we should terminate it.
# That could be done in this script, or in the Jenkinsfile that calls this
# script.
def start_ec2_instance(ami, region, security_group, iam_instance_profile):
    """ Start an EC2 instance and return its instance id """
    print("Starting EC2 instance...")
    cmd = ["aws", "ec2", "run-instances", "--image-id", ami,
           "--instance-type", INSTANCE_TYPE,
           "--security-groups", security_group,
           "--iam-instance-profile", f"Name={iam_instance_profile}",
           "--tag-specifications",
           "ResourceType=instance,Tags=[{Key=test,Value=max-sac-tps},{Key=ManagedBy,Value=ApplyLoadScript}]",
           "--query", "Instances[0].InstanceId",
           "--output", "text", "--region", region]
    instance_id = run_capture_output(cmd).decode().strip()
    print("Started EC2 instance with ID:", instance_id)

    # Wait for instance to be running
    print("Waiting for instance to be in 'running' state...")
    run(f"aws ec2 wait instance-running --instance-ids {instance_id} "
        f"--region {region}")
    return instance_id

def wait_for_ssm_agent(instance_id, region):
    """ Wait for SSM agent to be ready on the instance """
    print("Waiting for SSM agent to be ready...")
    for i in range(SSH_RETRIES):
        try:
            cmd = ["aws", "ssm", "describe-instance-information",
                   "--instance-information-filter-list",
                   f"key=InstanceIds,valueSet={instance_id}",
                   "--region", region]
            output = run_capture_output(cmd).decode().strip()
            info = json.loads(output)
            if info.get("InstanceInformationList"):
                print("SSM agent is ready.")
                return True
        except Exception as e:
            pass

        sleep_duration = 10
        print(f"SSM agent not ready yet, retrying ({i+1}/{SSH_RETRIES}) in "
              f"{sleep_duration} seconds...")
        time.sleep(sleep_duration)

    print("ERROR: SSM agent failed to become ready")
    exit(1)

def run_ssm_command(instance_id, region, command):
    """ Run a command on an EC2 instance via SSM """
    print(f"Running SSM command on {instance_id}: {command}")

    # Send command
    cmd = ["aws", "ssm", "send-command",
           "--instance-ids", instance_id,
           "--document-name", "AWS-RunShellScript",
           "--parameters", f"commands=['{command}']",
           "--region", region,
           "--query", "Command.CommandId",
           "--output", "text"]
    command_id = run_capture_output(cmd).decode().strip()

    # Wait for command to complete
    print(f"Waiting for command {command_id} to complete...")
    for i in range(30):
        time.sleep(5)
        cmd = ["aws", "ssm", "get-command-invocation",
               "--command-id", command_id,
               "--instance-id", instance_id,
               "--region", region,
               "--query", "Status",
               "--output", "text"]
        try:
            status = run_capture_output(cmd).decode().strip()
            if status in ["Success", "Failed", "Cancelled", "TimedOut"]:
                # Get output
                cmd = ["aws", "ssm", "get-command-invocation",
                       "--command-id", command_id,
                       "--instance-id", instance_id,
                       "--region", region]
                output = run_capture_output(cmd).decode().strip()
                result = json.loads(output)
                print("Command output:", result.get("StandardOutputContent", ""))
                if result.get("StandardErrorContent"):
                    print("Command error:", result.get("StandardErrorContent", ""))
                return status == "Success"
        except Exception as e:
            pass

    print("ERROR: Command timed out")
    return False

def copy_file_via_s3(instance_id, region, local_file, remote_path, s3_bucket):
    """ Copy a file to an EC2 instance via S3 and SSM """
    # Upload file to S3
    s3_key = f"tmp/{os.path.basename(local_file)}"
    print(f"Uploading {local_file} to s3://{s3_bucket}/{s3_key}...")
    run(f"aws s3 cp {local_file} s3://{s3_bucket}/{s3_key} --region {region}")

    # Download from S3 on the instance
    print(f"Downloading file to instance at {remote_path}...")
    download_cmd = f"aws s3 cp s3://{s3_bucket}/{s3_key} {remote_path}"
    run_ssm_command(instance_id, region, download_cmd)

    # Clean up S3 file
    run(f"aws s3 rm s3://{s3_bucket}/{s3_key} --region {region}")

def copy_files_to_instance(instance_id, region, s3_bucket):
    """ Copy files to the instance using SSM and S3 """
    print("Copying files to instance via S3...")

    # Copy this script
    copy_file_via_s3(instance_id, region, __file__,
                     f"{USER_DIR}/ApplyLoad.py", s3_bucket)

    # Copy config template
    copy_file_via_s3(instance_id, region, MAX_SAC_TEMPLATE,
                     f"{USER_DIR}/apply_load/max-sac-template.cfg", s3_bucket)

    # Create directory structure
    run_ssm_command(instance_id, region, f"mkdir -p {USER_DIR}/apply_load")

def install_script_on_instance(instance_id, region, s3_bucket):
    """ Install this script on the given EC2 instance via SSM. """
    # Wait for SSM agent to be ready
    wait_for_ssm_agent(instance_id, region)

    # Copy files
    copy_files_to_instance(instance_id, region, s3_bucket)

    return instance_id

def local_aws_init():
    """ Initialize an AWS instance for running apply-load from the instance
    itself. """
    # Mount ephemeral nvme drive such that docker uses it for storage
    run(f"sudo mkfs.ext4 {NVME_DRIVE}")
    run("sudo mkdir -p /var/lib/docker")
    run(f"sudo mount {NVME_DRIVE} /var/lib/docker")

    # Install docker
    run("sudo apt-get update")
    run("sudo apt-get install -y docker.io")

    # Allow regular ubuntu use to run docker commands
    run("sudo usermod -aG docker ubuntu")

def aws_init(ami, region, security_group, iam_instance_profile, s3_bucket):
    """ Create and initialize an AWS instance for running apply-load. """
    # Start instance
    instance_id = start_ec2_instance(ami, region, security_group, iam_instance_profile)

    # Install this script on the instance
    install_script_on_instance(instance_id, region, s3_bucket)

    # Remotely invoke local-aws-init on the instance
    run_ssm_command(instance_id, region,
                    f"cd {USER_DIR} && python3 ApplyLoad.py local-aws-init")

    # Print instance id for Jenkins to store
    print(f"{instance_id}")

def run_max_sac(cfg, image, iops):
    """ Run apply-load in max SAC TPS mode with the given config file. """
    with tempfile.NamedTemporaryFile() as cfg_out:
        cfg_out.write(cfg.encode())
        cfg_out.flush()
        iops_cmd = (f"--device-write-iops {NVME_DRIVE}:{iops} "
                    f"--device-read-iops {NVME_DRIVE}:{iops}"
                    if iops is not None else "")
        run(f"docker run --rm -v {cfg_out.name}:/config.cfg {iops_cmd} {image} "
            "apply-load --mode max-sac-tps --console --conf /config.cfg")

def generate_cfg(clusters, batch_size):
    """ Generate a configuration file for max SAC TPS mode. """
    with open(MAX_SAC_TEMPLATE, "r") as template_file:
        template = template_file.read()
        return template.format(clusters=clusters, batch_size=batch_size)

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Helper script to run apply-load tests on AWS")

    subparsers = parser.add_subparsers(dest="mode", required=True)

    aws_init_parser = subparsers.add_parser(
        "aws-init",
        help="Create and initialize an AWS instance for running apply-load.")
    aws_init_parser.add_argument(
        "--ubuntu-ami", type=str,
        help="AMI ID to use. Must be an Ubuntu image.")
    aws_init_parser.add_argument("--region", type=str, help="AWS region to use.")
    aws_init_parser.add_argument("--security-group", type=str,
                         help="AWS security group to use.")
    aws_init_parser.add_argument("--iam-instance-profile", type=str,
                         help="IAM instance profile to use.")
    aws_init_parser.add_argument("--s3-bucket", type=str,
                         help="S3 bucket to use for file transfer.")

    subparsers.add_parser(
        "local-aws-init",
        help="Initialize the local AWS instance for running apply-load.")

    run_max_sac_parser = subparsers.add_parser(
        "max-sac", help="Run apply-load in max SAC TPS mode.")
    run_max_sac_parser.add_argument(
        "--image", type=str, required=True, help="Docker image to use.")
    run_max_sac_parser.add_argument(
        "--clusters", type=int, required=True,
        help="Number of transaction clusters (threads).")
    run_max_sac_parser.add_argument(
        "--batch-size", type=int, required=True,
        help="Batch size for transactions.")
    run_max_sac_parser.add_argument(
        "--iops", type=int, required=False, help="IOPS limit for the disk.")
    args = parser.parse_args()

    if args.mode == "aws-init":
        aws_init(args.ubuntu_ami, args.region, args.security_group,
                 args.iam_instance_profile, args.s3_bucket)
    elif args.mode == "local-aws-init":
        local_aws_init()
    elif args.mode == "max-sac":
        cfg = generate_cfg(args.clusters, args.batch_size)
        run_max_sac(cfg, args.image, args.iops)
    else:
        print(f"Unknown mode: {args.mode}")
        exit(1)
