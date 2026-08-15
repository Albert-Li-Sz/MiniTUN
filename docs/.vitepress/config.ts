import { defineConfig } from 'vitepress'

const sharedThemeConfig = {
  logo: '/logo.svg',
  siteTitle: 'MiniTun',
  socialLinks: [
    { icon: 'github', link: 'https://github.com/Albert-Li-Sz/MiniTUN' }
  ],
  search: {
    provider: 'local',
    options: {
      translations: {
        button: {
          buttonText: '搜索',
          buttonAriaLabel: '搜索'
        },
        modal: {
          displayDetails: '显示详细列表',
          resetButtonTitle: '重置搜索',
          backButtonTitle: '关闭搜索',
          noResultsText: '未找到结果',
          footer: {
            selectText: '选择',
            selectKeyAriaLabel: '回车',
            navigateText: '切换',
            navigateUpKeyAriaLabel: '上箭头',
            navigateDownKeyAriaLabel: '下箭头',
            closeText: '关闭',
            closeKeyAriaLabel: 'Esc 键'
          }
        }
      },
      locales: {
        en: {
          translations: {
            button: {
              buttonText: 'Search',
              buttonAriaLabel: 'Search'
            },
            modal: {
              displayDetails: 'Display detailed list',
              resetButtonTitle: 'Reset search',
              backButtonTitle: 'Close search',
              noResultsText: 'No results for',
              footer: {
                selectText: 'to select',
                selectKeyAriaLabel: 'enter',
                navigateText: 'to navigate',
                navigateUpKeyAriaLabel: 'up arrow',
                navigateDownKeyAriaLabel: 'down arrow',
                closeText: 'to close',
                closeKeyAriaLabel: 'escape'
              }
            }
          }
        }
      }
    }
  },
  footer: {
    message: 'Released under the MIT License.',
    copyright: 'Copyright © 2026 MiniTun maintainers'
  }
}

export default defineConfig({
  title: 'MiniTun',
  description: '资源占用最小的自托管内网穿透工具',
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
    ['meta', { property: 'og:description', content: '资源占用最小的自托管内网穿透工具' }]
  ],
  themeConfig: {
    ...sharedThemeConfig
  },
  locales: {
    root: {
      label: '中文',
      lang: 'zh-CN',
      link: '/',
      title: 'MiniTun',
      description: '资源占用最小的自托管内网穿透工具',
      themeConfig: {
        nav: [
          { text: '指南', link: '/' },
          { text: '安装', link: '/installation' },
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
              { text: '安装指南', link: '/installation' },
              { text: '命令行界面', link: '/cli' },
              { text: '配置与客户端策略', link: '/configuration' },
              { text: '运维与可观测性', link: '/operations' },
              { text: 'Let\'s Encrypt 证书', link: '/letsencrypt' },
              { text: '开发文档', link: '/development' },
              { text: '变更日志', link: '/changelog' }
            ]
          },
          {
            text: '技术参考',
            items: [
              { text: '系统架构', link: '/architecture' },
              { text: 'Remote Protocol v2', link: '/protocol' },
              { text: '本地控制与远程协议 SDK', link: '/sdk' },
              { text: '性能与浸泡门禁', link: '/performance' }
            ]
          }
        ],
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
        }
      }
    },
    en: {
      label: 'English',
      lang: 'en-US',
      link: '/en/',
      title: 'MiniTun',
      description: 'A minimal-footprint self-hosted intranet penetration (reverse tunnel) tool',
      themeConfig: {
        nav: [
          { text: 'Guide', link: '/en/' },
          { text: 'Installation', link: '/en/installation' },
          { text: 'CLI', link: '/en/cli' },
          { text: 'Configuration', link: '/en/configuration' },
          { text: 'Architecture', link: '/en/architecture' },
          { text: 'Protocol', link: '/en/protocol' },
          { text: 'SDK', link: '/en/sdk' },
          { text: 'Development', link: '/en/development' },
          { text: 'Changelog', link: '/en/changelog' }
        ],
        sidebar: [
          {
            text: 'User Guide',
            items: [
              { text: 'Home', link: '/en/' },
              { text: 'Installation Guide', link: '/en/installation' },
              { text: 'Command Line Interface', link: '/en/cli' },
              { text: 'Configuration & Client Policies', link: '/en/configuration' },
              { text: 'Operations & Observability', link: '/en/operations' },
              { text: 'Let\'s Encrypt Certificates', link: '/en/letsencrypt' },
              { text: 'Development Guide', link: '/en/development' },
              { text: 'Changelog', link: '/en/changelog' }
            ]
          },
          {
            text: 'Technical Reference',
            items: [
              { text: 'System Architecture', link: '/en/architecture' },
              { text: 'Remote Protocol v2', link: '/en/protocol' },
              { text: 'Local Control & Remote Protocol SDK', link: '/en/sdk' },
              { text: 'Performance & Soak Gates', link: '/en/performance' }
            ]
          }
        ],
        outline: {
          label: 'On this page',
          level: [2, 3]
        },
        docFooter: {
          prev: 'Previous page',
          next: 'Next page'
        },
        lastUpdated: {
          text: 'Last updated',
          formatOptions: {
            dateStyle: 'medium',
            timeStyle: 'short'
          }
        }
      }
    }
  }
})
