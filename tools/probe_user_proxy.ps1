function Get-UserProxySnapshot {
    if (-not ('BlankBackendLab.ProxyProbe' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BlankBackendLab {
    public static class ProxyProbe {
        [StructLayout(LayoutKind.Sequential)]
        private struct Option { public uint Kind; public IntPtr Value; }
        [StructLayout(LayoutKind.Sequential)]
        private struct Options {
            public uint Size; public IntPtr Connection; public uint Count;
            public uint Error; public IntPtr Values;
        }
        [DllImport("wininet.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool InternetQueryOptionW(IntPtr internet, uint option, ref Options value, ref uint length);
        [DllImport("kernel32.dll")]
        private static extern IntPtr GlobalFree(IntPtr memory);
        public static string[] Read() {
            var stride = Marshal.SizeOf(typeof(Option));
            var buffer = Marshal.AllocHGlobal(stride * 4);
            var kinds = new uint[] { 10, 2, 3, 4 };
            for (var index = 0; index < 4; ++index) {
                Marshal.StructureToPtr(new Option { Kind = kinds[index] }, IntPtr.Add(buffer, stride * index), false);
            }
            try {
                var options = new Options { Size = (uint)Marshal.SizeOf(typeof(Options)), Count = 4, Values = buffer };
                var size = options.Size;
                if (!InternetQueryOptionW(IntPtr.Zero, 75, ref options, ref size)) {
                    throw new System.ComponentModel.Win32Exception();
                }
                var result = new string[4];
                for (var index = 0; index < 4; ++index) {
                    var value = (Option)Marshal.PtrToStructure(IntPtr.Add(buffer, stride * index), typeof(Option));
                    result[index] = index == 0 ? unchecked((uint)value.Value.ToInt64()).ToString() :
                        (value.Value == IntPtr.Zero ? "" : Marshal.PtrToStringUni(value.Value));
                }
                return result;
            } finally {
                for (var index = 1; index < 4; ++index) {
                    var value = (Option)Marshal.PtrToStructure(IntPtr.Add(buffer, stride * index), typeof(Option));
                    if (value.Value != IntPtr.Zero) { GlobalFree(value.Value); }
                }
                Marshal.FreeHGlobal(buffer);
            }
        }
    }
}
'@
    }
    $fields = [BlankBackendLab.ProxyProbe]::Read()
    [pscustomobject]@{ Flags = $fields[0]; Server = $fields[1]; Bypass = $fields[2]; Pac = $fields[3] }
}
