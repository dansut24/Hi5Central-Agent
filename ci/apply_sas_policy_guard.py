from pathlib import Path

p = Path('Agent/chatpass_agent/src/platform/windows/windows_service.cpp')
s = p.read_text(encoding='utf-8')
old = '''            if (backup.hadValue && ((backup.oldValue & 0x1u) != 0)) {
                CadLog("temporary SoftwareSASGeneration already allows Services; leaving existing value unchanged");
                RegCloseKey(key);
                return true;
            }

            DWORD newValue = backup.hadValue ? (backup.oldValue | 0x1u) : 1u;
'''
new = '''            if (backup.hadValue) {
                if ((backup.oldValue & 0x1u) != 0) {
                    CadLog("SoftwareSASGeneration already allows Services; existing administrator policy left unchanged");
                    RegCloseKey(key);
                    return true;
                }
                CadLog("SoftwareSASGeneration explicitly configured without Services; respecting administrator policy");
                RegCloseKey(key);
                return false;
            }

            DWORD newValue = 1u;
'''
if old not in s:
    raise RuntimeError('expected SoftwareSASGeneration block not found')
s = s.replace(old, new, 1)
s = s.replace('CAD_BUILD=Temporary-SoftwareSASGeneration-SendSAS active=true', 'CAD_BUILD=PolicyRespecting-Temporary-SoftwareSASGeneration-SendSAS active=true', 1)
s = s.replace('service SendSAS skipped because temporary SoftwareSASGeneration could not be enabled', 'service SendSAS skipped because Services SAS is blocked by policy or unavailable', 1)
p.write_text(s, encoding='utf-8')
print('SAS policy guard applied')
