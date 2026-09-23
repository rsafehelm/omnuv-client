package main

import (
	"os"

	"golang.org/x/sys/windows"
)

// **SYSTEM and Administrators, and nobody else.** The directory holds this
// device's WireGuard private key. MkdirAll's 0o700 means nothing on Windows,
// so it inherited %ProgramData%'s ACL, under which BUILTIN\Users can read and
// execute: any local account could read the key and impersonate the machine
// on its network. This replaces the DACL with a protected one, inherited by
// everything inside, including what is already there.
const secureDirSDDL = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"

func secureDir(dir string) error {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	sd, err := windows.SecurityDescriptorFromString(secureDirSDDL)
	if err != nil {
		return err
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return err
	}
	return windows.SetNamedSecurityInfo(dir, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		nil, nil, dacl, nil)
}
