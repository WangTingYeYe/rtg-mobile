# 项目约定

- 当前支持 Android arm64。构建与测试入口见 README.md。
- 游戏文件、改写模块、着色器转储、字体、存档和签名密钥只放在被忽略的本地目录中；公开提交前运行 `python3 tools/check_public_tree.py --staged`。
- 兼容层源自 d2r-mobile；借鉴实现时保留版权声明与第三方许可证，来源见 THIRD_PARTY_NOTICES.md。
- 不把显示刷新率当作游戏 FPS，也不以 CPU 单测替代真机图形、音频和生命周期验证。
