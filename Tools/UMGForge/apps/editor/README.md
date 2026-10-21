# UMGForge Editor

面向美术的 UMG 可视化编辑器 MVP。

## 功能

- 左侧层级树，展示节点结构
- 中间 1920x1080 画布，支持缩放、平移、点击选中
- 右侧属性面板，编辑 Slot/外观/交互
- 工具栏提供常用控件添加、撤销重做、保存/加载
- 支持 Button OnClicked 配置 SetSwitcherIndex/SetVisibility/EmitEvent
- 底部问题列表（基础预检）
- 内置示例文档

## 技术栈

- React 18 + TypeScript
- Vite 6
- Vitest (测试)
- 纯 DOM/CSS 渲染（Canvas/HBox/VBox/Overlay 按 UMG 语义布局）

## 开发

```bash
npm install
npm run dev      # 启动开发服务器
npm run build    # 构建生产版本
npm test         # 运行测试
```

## 架构

- `src/core-adapter/` - 唯一适配层，封装文档/命令 API
- `src/state/` - React 状态管理
- `src/ui/` - UI 组件（Toolbar/Hierarchy/Canvas/Inspector）
- 所有文档修改经由 Command API，支持撤销重做

## 注意

- 不实现 UE 导入导出
- 不使用 AI 辅助功能
- 文本文件统一使用 CRLF 换行符
