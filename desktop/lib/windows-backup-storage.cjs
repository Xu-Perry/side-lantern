const {execFile} = require('node:child_process');
const path = require('node:path');

function apply(method, bytes) {
  // Use CurrentUser DPAPI directly, so a backup can move between profiles of
  // the same Windows user. Electron's async vault has a per-profile key.
  const script = `
    $ErrorActionPreference = 'Stop'
    Add-Type -AssemblyName System.Security
    $data = [Convert]::FromBase64String([Console]::In.ReadToEnd())
    $entropy = [Text.Encoding]::UTF8.GetBytes('SideLantern.Migration.1')
    $result = [Security.Cryptography.ProtectedData]::${method}(
      $data, $entropy, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    [Console]::Write([Convert]::ToBase64String($result))
  `;
  const powershell = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32',
    'WindowsPowerShell', 'v1.0', 'powershell.exe');
  const encodedScript = Buffer.from(script, 'utf16le').toString('base64');
  return new Promise((resolve, reject) => {
    const child = execFile(powershell, ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', encodedScript],
      {windowsHide: true, timeout: 60000, maxBuffer: 100 * 1024 * 1024}, (error, stdout) => {
        if (error) {
          reject(new Error(method === 'Protect' ? 'Windows 无法加密迁移备份。'
            : '无法解密迁移备份，请使用导出备份时的 Windows 帐号。'));
          return;
        }
        resolve(Buffer.from(stdout.trim(), 'base64'));
      });
    child.stdin.on('error', () => {});
    child.stdin.end(bytes.toString('base64'));
  });
}

const windowsBackupStorage = {
  encryptString: value => apply('Protect', Buffer.from(value, 'utf8')),
  async decryptString(value) { return (await apply('Unprotect', value)).toString('utf8'); }
};

module.exports = {windowsBackupStorage};
