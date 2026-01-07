#!/usr/bin/env python3

import os
import subprocess
import tempfile
import time

# Instance type to use. Matches SDF validator instance type.
INSTANCE_TYPE = 'c5d.2xlarge'

# Key pair name and file for SSH access
# TODO: Fill these in with the proper values
KEY_NAME = 'max-sac-test-key'
KEY_FILE = 'max-sac-test-key.pem'

# Directory containing helper files for this script
APPLY_LOAD_SCRIPT_DIR = os.path.join(os.path.dirname(__file__), "apply_load")

# Path to the max SAC template configuration file
MAX_SAC_TEMPLATE = os.path.join(APPLY_LOAD_SCRIPT_DIR, "max-sac-template.cfg")

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

def create_key_pair(region):
    """ Create an EC2 key pair and save the private key to KEY_FILE. """
    print("Creating EC2 key pair...")
    cmd = ["aws", "ec2", "create-key-pair", "--key-name", KEY_NAME,
           "--query", "KeyMaterial", "--output", "text", "--region", region]
    private_key = run_capture_output(cmd).decode().strip()
    with open(KEY_FILE, "w") as key_file:
        key_file.write(private_key)
    os.chmod(KEY_FILE, 0o400)
    print(f"Saved private key to {KEY_FILE}")

# TODO: If anything fails AFTER starting the instance, we should terminate it.
# That could be done in this script, or in the Jenkinsfile that calls this
# script.
def start_ec2_instance(ami, region, security_group):
    """ Start an EC2 instance and return its instance id """
    print("Starting EC2 instance...")
    cmd = ["aws", "ec2", "run-instances", "--image-id", ami,
           "--instance-type", INSTANCE_TYPE,
           "--security-groups", security_group,
           "--key-name", KEY_NAME, "--query", "Instances[0].InstanceId",
           "--output", "text", "--region", region]
    instance_id = run_capture_output(cmd).decode().strip()
    print("Started EC2 instance with ID:", instance_id)

    # Wait for instance to be running
    print("Waiting for instance to be in 'running' state...")
    run(f"aws ec2 wait instance-running --instance-ids {instance_id} "
        f"--region {region}")
    return instance_id

def install_script_on_instance(instance_id, region):
    """ Install this script on the given EC2 instance. """
    # Get the instance's public IP address
    # TODO: remove region
    ip = run_capture_output(
        ["aws", "ec2", "describe-instances", "--instance-ids", instance_id,
        "--query", "Reservations[0].Instances[0].PublicIpAddress",
        "--output", "text", "--region", region]).decode().strip()
    print("Instance public IP:", ip)

    # Wait for SSH to be available
    print("Checking SSH availability...")
    for i in range(SSH_RETRIES):
        res = run(f"ssh -o StrictHostKeyChecking=no -i {KEY_FILE} "
                  "-o ConnectTimeout=5 "
                  f"ubuntu@{ip} 'true'",
                  exit_on_fail=(i == SSH_RETRIES - 1))
        if res:
            break
        sleep_duration = 10
        print(f"SSH not available yet, retrying ({i+1}/{SSH_RETRIES}) in "
              f"{sleep_duration} seconds...")
        time.sleep(sleep_duration)
    print("SSH is available.")

    # Copy this script and the apply-load directory to the instance
    scp_base = f"scp -i {KEY_FILE} -o StrictHostKeyChecking=no"
    dest = f"ubuntu@{ip}:"
    run(f"{scp_base} {__file__} {dest}")
    run(f"{scp_base} -r {APPLY_LOAD_SCRIPT_DIR} {dest}")

    return ip

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

def aws_init(ami, region, security_group):
    """ Create and initialize an AWS instance for running apply-load. """
    # Create key pair
    create_key_pair(region)

    # Start instance
    instance_id = start_ec2_instance(ami, region, security_group)

    # Install this script on the instance
    ip = install_script_on_instance(instance_id, region)

    # Remotely invoke local-aws-init on the instance
    run(f"ssh -o StrictHostKeyChecking=no -i {KEY_FILE} "
        f"ubuntu@{ip} 'python3 ApplyLoad.py local-aws-init'")

    # Print instance id and ip for Jenkins to store
    print(f"{instance_id},{ip}")

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

    aws_init = subparsers.add_parser(
        "aws-init",
        help="Create and initialize an AWS instance for running apply-load.")
    aws_init.add_argument(
        "--ubuntu-ami", type=str,
        help="AMI ID to use. Must be an Ubuntu image.")
    aws_init.add_argument("--region", type=str, help="AWS region to use.")
    aws_init.add_argument("--security-group", type=str,
                         help="AWS security group to use.")

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
        aws_init(args.ubuntu_ami, args.region, args.security_group)
    elif args.mode == "local-aws-init":
        local_aws_init()
    elif args.mode == "max-sac":
        cfg = generate_cfg(args.clusters, args.batch_size)
        run_max_sac(cfg, args.image, args.iops)
    else:
        print(f"Unknown mode: {args.mode}")
        exit(1)