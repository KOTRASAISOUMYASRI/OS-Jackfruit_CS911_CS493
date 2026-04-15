1. Team Information
Name: <Your Name>
SRN: <Your SRN>

(Add more members if applicable)

2. Build, Load, and Run Instructions
🔹 Environment
Ubuntu 22.04 / 24.04 VM
Secure Boot OFF
Dependencies installed:
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
🔹 Build
cd boilerplate
make clean
make
🔹 Load Kernel Module
sudo insmod monitor.ko
lsmod | grep monitor
🔹 Run Containers
sudo ./engine run alpha ./cpu_hog
sudo ./engine run beta ./cpu_hog
🔹 List Containers
./engine list
🔹 Run Workloads
./cpu_hog
./memory_hog
./io_pulse
🔹 Stop Containers
./engine stop alpha
./engine stop beta
🔹 View Logs
cat runtime.log
dmesg | tail
🔹 Cleanup
sudo rmmod monitor
3. Demo with Screenshots
3.1 Multi-container Supervision

Description:
Two or more containers running simultaneously.

📸 [Insert Screenshot Here]
(Show multiple containers running — cpu_hog, memory_hog, etc.)

3.2 Metadata Tracking

Description:
Listing running containers and their PIDs.

📸 [Insert Screenshot Here]
(Show ./engine list OR ps -ef | grep hog output)

3.3 Logging System

Description:
Container lifecycle events recorded in log file.

📸 [Insert Screenshot Here]
(Show cat runtime.log output)

3.4 CLI and IPC

Description:
CLI command issued and kernel module receives PID via ioctl.

📸 [Insert Screenshot Here]
(Show engine run + dmesg | tail with PID message)

3.5 Soft-limit Warning

Description:
(Not fully implemented)

📸 [Insert Screenshot Here – Optional]

3.6 Hard-limit Enforcement

Description:
(Not implemented in this version)

📸 [Insert Screenshot Here – Optional]

3.7 Scheduling Experiment

Description:
Comparison of CPU, memory, and I/O workloads.

📸 [Insert Screenshot Here]
(Show top, free -h, ps outputs)

3.8 Clean Teardown

Description:
Containers stopped and no zombie processes remain.

📸 [Insert Screenshot Here]
(Show ps aux | grep engine after stopping containers)

4. Engineering Analysis
Linux uses namespaces (CLONE_NEWPID, CLONE_NEWUTS, CLONE_NEWNS) to isolate processes.
chroot() provides filesystem isolation for containers.
Kernel modules operate in privileged mode and can monitor system-level events.
ioctl enables communication between user-space and kernel-space.
Linux scheduler distributes CPU time among processes based on workload type.
5. Design Decisions and Tradeoffs
🔹 Namespace Isolation
Choice: Used clone() with namespaces
Tradeoff: Limited isolation compared to full container runtimes
Reason: Simpler implementation suitable for project scope
🔹 Supervisor Architecture
Choice: Simple CLI-based supervisor
Tradeoff: No persistent state across runs
Reason: Reduced complexity
🔹 IPC and Logging
Choice: Used ioctl + file logging
Tradeoff: Minimal communication features
Reason: Lightweight and easy to debug
🔹 Kernel Monitor
Choice: Basic kernel module tracking PIDs
Tradeoff: No deep memory enforcement
Reason: Focus on integration rather than complexity
🔹 Scheduling Experiments
Choice: Used CPU, memory, and I/O workloads
Tradeoff: No advanced metrics collection
Reason: Demonstrates observable behavior clearly
6. Scheduler Experiment Results
🔹 Observations
Workload	Behavior
CPU Hog	High CPU usage, continuous execution
Memory Hog	Increased RAM usage
IO Pulse	Intermittent CPU + I/O activity
🔹 Analysis
CPU-bound processes dominate CPU usage
Memory-heavy processes increase system load
I/O-bound processes show burst behavior
Scheduler balances resources among processes
🎯 Conclusion
Implemented a basic container runtime in C
Demonstrated process isolation using Linux primitives
Integrated kernel module with user-space runtime
Observed scheduling behavior using workloads
