namespace Hi5Central.AppPortal.Broker;

public static class AppPortalBrokerFactory
{
    public static IAppPortalBroker Create()
    {
        if (OperatingSystem.IsWindows())
        {
            return new WindowsNamedPipeAppPortalBroker();
        }

        throw new PlatformNotSupportedException(
            "A Hi5Central App Portal broker is not available for this operating system yet.");
    }
}