from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

start = text.find('                if (type == "files_mkdir_request") {')
end = text.find('                if (type == "files_rename_request") {', start)

if start == -1 or end == -1:
    raise SystemExit("Could not find mkdir block boundaries")

new_block = r'''                if (type == "files_mkdir_request") {
                    if (nameValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Folder name is required");
                        return;
                    }

                    if (nameValue.find("\\") != std::string::npos ||
                        nameValue.find("/") != std::string::npos ||
                        nameValue.find(":") != std::string::npos ||
                        nameValue == "." ||
                        nameValue == "..") {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Invalid folder name");
                        return;
                    }

                    std::string basePath = refreshPath.empty() ? pathValue : refreshPath;
                    if (basePath.empty()) {
                        basePath = "C:\\";
                    }

                    if (!basePath.empty() && basePath.back() != '\\' && basePath.back() != '/') {
                        basePath += "\\";
                    }

                    const std::string fullPath = basePath + nameValue;

                    LogI("live files mkdir requested session=" + sessionId + " path=" + fullPath);

                    BOOL created = CreateDirectoryA(fullPath.c_str(), nullptr);
                    DWORD err = created ? ERROR_SUCCESS : GetLastError();

                    if (!created && err == ERROR_ALREADY_EXISTS) {
                        DWORD attrs = GetFileAttributesA(fullPath.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            SendFilesActionResult(sessionId, type, true, refreshPath, "Folder already exists");
                            return;
                        }
                    }

                    if (!created && err != ERROR_SUCCESS) {
                        LogW("live files mkdir failed session=" + sessionId +
                             " path=" + fullPath +
                             " err=" + std::to_string(err));

                        SendFilesActionResult(
                            sessionId,
                            type,
                            false,
                            refreshPath,
                            "",
                            "Create folder failed. Windows error " + std::to_string(err)
                        );
                        return;
                    }

                    LogI("live files mkdir completed session=" + sessionId + " path=" + fullPath);

                    SendFilesActionResult(
                        sessionId,
                        type,
                        true,
                        refreshPath,
                        "Folder created"
                    );

                    return;
                }

'''

text = text[:start] + new_block + text[end:]
path.write_text(text, encoding="utf-8")
