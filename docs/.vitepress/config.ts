import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'MiniTun',
  description: '面向 Linux 团队自托管的安全 TCP 反向隧道',
  lang: 'zh-CN',
  base: '/MiniTUN/',
  cleanUrls: true,
  lastUpdated: true,
  markdown: {
    lineNumbers: true,
    theme: {
      light: 'github-light',
      dark: 'github-dark'
    }
  },
  head: [
    ['meta', { name: 'theme-color', content: '#0f766e' }],
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:title', content: 'MiniTun' }],
    ['meta', { property: 'og:description', content: '面向 Linux 团队自托管的安全 TCP 反向隧道' }]
  ],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'MiniTun',
    nav: [
      { text: '指南', link: '/' },
      { text: 'CLI', link: '/cli' },
      { text: '配置', link: '/configuration' },
      { text: '架构', link: '/architecture' },
      { text: '协议', link: '/protocol' },
      { text: 'SDK', link: '/sdk' },
      { text: '开发', link: '/development' },
      { text: '变更日志', link: '/changelog' }
    ],
    sidebar: [
      {
        text: '使用指南',
        items: [
          { text: '项目首页', link: '/' },
          { text: '命令行界面', link: '/cli' },
          { text: '配置与客户端策略', link: '/configuration' },
          { text: '运维与可观测性', link: '/operations' },
          { text: 'v1 迁移指南', link: '/migration-v1' },
          { text: '开发文档', link: '/development' },
          { text: '变更日志', link: '/changelog' }
        ]
      },
      {
        text: '技术参考',
        items: [
          { text: '系统架构', link: '/architecture' },
          { text: 'Remote Protocol v2', link: '/protocol' },
          { text: '本地控制 SDK', link: '/sdk' },
          { text: '性能与浸泡门禁', link: '/performance' }
        ]
      }
    ],
    socialLinks: [
      { icon: 'github', link: 'https://github.com/LMTINSUZHOU/MiniTUN' }
    ],
    search: {
      provider: 'local'
    },
    outline: {
      label: '本页目录',
      level: [2, 3]
    },
    docFooter: {
      prev: '上一页',
      next: '下一页'
    },
    lastUpdated: {
      text: '最后更新',
      formatOptions: {
        dateStyle: 'medium',
        timeStyle: 'short'
      }
    },
    footer: {
      message: 'Released under the MIT License.',
      copyright: 'Copyright © 2026 MiniTun maintainers'
    }
  }
})
