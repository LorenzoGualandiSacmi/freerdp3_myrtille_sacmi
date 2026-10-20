using System.Runtime.InteropServices;

// Same x86 P/Invoke entry point as Myrtille.RDP, without gateway dependencies.
internal static class MyrtilleNativeHost
{
    [DllImport("wfreerdp-client.dll")]
    private static extern int StartRdpClient();
    private static int Main() { return StartRdpClient(); }
}
