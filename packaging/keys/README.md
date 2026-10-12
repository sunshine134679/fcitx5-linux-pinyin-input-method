# ModernIME 软件源签名密钥管理与轮换规范 (`packaging/keys/`)

本目录用于管理 ModernIME APT / DNF 软件源的受信公钥集合与平滑密钥轮换窗口。

## 1. 持久化发布签名密钥（Production Signing Key）

正式发布（GitHub Actions `Release` 工作流）**严禁使用临时密钥（`--ephemeral-gpg`）**，必须通过 GitHub Actions Secrets 提供持久化签名私钥：

- `MODERNIME_GPG_PRIVATE_KEY`（必填）：主签名私钥（ASCII Armored 格式）。
- `MODERNIME_GPG_KEY_ID`（可选）：指定主签名密钥指纹或长 Key ID（未设置时自动取导入的第一个私钥）。
- `MODERNIME_GPG_ROTATION_PRIVATE_KEY`（可选，轮换过渡期使用）：新/旧过渡签名私钥（ASCII Armored 格式）。
- `MODERNIME_GPG_ROTATION_KEY_ID`（可选）：轮换过渡密钥 ID。

## 2. 首次安装信任建立（TOFU + 指纹核验）

用户首次安装 `/usr/share/keyrings/modernime-archive-keyring.gpg`（Ubuntu/Debian）或 `/etc/pki/rpm-gpg/RPM-GPG-KEY-modernime`（openEuler）时，可通过随仓库发布的 `KEY-FINGERPRINTS.txt` 及 GitHub Release 的 `SHA256SUMS.txt` 校验公钥完整性与 40 位十六进制 OpenPGP 指纹：

```bash
# 核验 APT 二进制密钥环指纹
gpg --show-keys --with-fingerprint /usr/share/keyrings/modernime-archive-keyring.gpg

# 核验 RPM ASCII 公钥指纹
gpg --show-keys --with-fingerprint /etc/pki/rpm-gpg/RPM-GPG-KEY-modernime
```

## 3. 密钥轮换流程（Zero-Downtime Key Rotation）

当需要轮换发布签名密钥（例如旧密钥即将到期）时，按以下三步执行：

1. **阶段一（预分发新公钥）**：
   - 将新生成的公钥（`.asc`）放入 `packaging/keys/trusted-pubkeys.d/` 并提交。
   - `build_apt_repo.sh` 与 `build_rpm_repo.sh` 会自动将 `trusted-pubkeys.d/*.asc` 合并导出到 `modernime-archive-keyring.gpg` 与 `RPM-GPG-KEY-modernime` 中。
2. **阶段二（双密钥并签过渡期）**：
   - 在 GitHub Secrets 中同时配置 `MODERNIME_GPG_PRIVATE_KEY`（新私钥）与 `MODERNIME_GPG_ROTATION_PRIVATE_KEY`（旧私钥）。
   - `build_apt_repo.sh` 会对 `InRelease` 与 `Release.gpg` 同时附加新旧两个密钥的签名；`build_rpm_repo.sh` 会对 `repomd.xml.asc` 同时附加新旧两个密钥的签名。
   - 已安装旧密钥环的客户端无需手动干预即可验证通过并升级包与新密钥环。
3. **阶段三（退役旧密钥）**：
   - 过渡期结束后，从 `packaging/keys/trusted-pubkeys.d/` 及 `MODERNIME_GPG_ROTATION_PRIVATE_KEY` 移除已退役的旧密钥。
