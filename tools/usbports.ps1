# Reads the connection status of every port of every USB hub of this PC, the
# way USBView does (IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX per port).
# Useful when a board under test no longer shows up: a port that reports
# "DeviceCausedOvercurrent" was shut off by the PC after a short, the board
# is most likely fine. Run in a normal PowerShell, no admin rights needed.
$src = @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class UsbPorts {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr templ);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inSize, byte[] outBuf, int outSize, out int returned, IntPtr ov);
  const uint IOCTL_NODE_INFO = 0x220408;
  const uint IOCTL_CONN_INFO_EX = 0x220448;
  public static string Scan(string path) {
    var sb = new System.Text.StringBuilder();
    using (var h = CreateFile(path, 0x40000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
      if (h.IsInvalid) return "  cannot open (" + Marshal.GetLastWin32Error() + ")\n";
      byte[] ni = new byte[76]; int ret;
      if (!DeviceIoControl(h, IOCTL_NODE_INFO, ni, ni.Length, ni, ni.Length, out ret, IntPtr.Zero)) return "  node info failed\n";
      int ports = ni[6];   // USB_HUB_DESCRIPTOR.bNumberOfPorts, after NodeType(4)+bLength+bType
      for (int p = 1; p <= ports; p++) {
        byte[] ci = new byte[35 + 30*32];
        BitConverter.GetBytes(p).CopyTo(ci, 0);
        if (!DeviceIoControl(h, IOCTL_CONN_INFO_EX, ci, ci.Length, ci, ci.Length, out ret, IntPtr.Zero)) { sb.AppendFormat("  port {0,2}: query failed\n", p); continue; }
        ushort vid = BitConverter.ToUInt16(ci, 4 + 8), pid = BitConverter.ToUInt16(ci, 4 + 10);
        byte speed = ci[4 + 18 + 1];
        int status = BitConverter.ToInt32(ci, 4 + 18 + 1 + 1 + 1 + 1 + 1 + 4);
        sb.AppendFormat("  port {0,2}: status {1} {2}  vid {3:x4} pid {4:x4} speed {5}\n", p, status, Name(status), vid, pid, speed);
      }
    }
    return sb.ToString();
  }
  static string Name(int s) {
    string[] n = { "NoDeviceConnected", "DeviceConnected", "DeviceFailedEnumeration", "DeviceGeneralFailure", "DeviceCausedOvercurrent", "DeviceNotEnoughPower", "DeviceNotEnoughBandwidth", "DeviceHubNestedTooDeeply", "DeviceInLegacyHub", "DeviceEnumerating", "DeviceReset" };
    return (s >= 0 && s < n.Length) ? n[s] : "?";
  }
}
"@
Add-Type -TypeDefinition $src -ErrorAction Stop
$guid = '{f18a0e88-c30c-11d0-8815-00a0c906bed8}'
$hubs = Get-PnpDevice -PresentOnly -Class USB | Where-Object { $_.FriendlyName -match 'Hub' }
foreach ($hub in $hubs) {
  $path = '\\?\' + ($hub.InstanceId -replace '\\', '#') + '#' + $guid
  "{0}  [{1}]" -f $hub.FriendlyName, $hub.InstanceId
  [UsbPorts]::Scan($path)
}
