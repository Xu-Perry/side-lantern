using System;
using System.Globalization;
using System.Text;

namespace SideLantern.HostService
{
    internal static class HostProtocol
    {
        internal static bool TryPort(string command, out int port)
        {
            port = 0;
            return command != null && command.StartsWith("OPEN ", StringComparison.Ordinal)
                && Int32.TryParse(command.Substring(5), NumberStyles.None, CultureInfo.InvariantCulture, out port)
                && port >= 1029 && port <= 65514;
        }

        internal static string Quote(string value)
        {
            var result = new StringBuilder("\"");
            int escapes = 0;
            foreach (char character in value)
            {
                if (character == '\\')
                {
                    escapes++;
                    continue;
                }
                if (character == '"') result.Append('\\', escapes + 1);
                result.Append('\\', escapes).Append(character);
                escapes = 0;
            }
            return result.Append('\\', escapes * 2).Append('"').ToString();
        }
    }
}
