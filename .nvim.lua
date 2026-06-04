vim.lsp.enable('clangd')
vim.lsp.enable('cmake')

vim.lsp.config('slangd', {
  settings = {
    slang = {
      searchInAllWorkspaceDirectories = true,
      additionalSearchPaths = {
        'build/linux-clang/bin/Debug/shaders',
        'Source/Falcor',
        'external/RTXDI/rtxdi-sdk/include',
      },
      inlayHints = {
        deducedTypes = false,
        parameterNames = false,
      }
    }
  },
})
vim.lsp.enable('slangd')

vim.lsp.config('ty', {
  cmd = { 'ty', 'server' },
  filetypes = { 'python' },
  settings = {
    ty = {
      configuration = {
        environment = {
          ['extra-paths'] = {
            'build/linux-clang/bin/Debug/python',
          },
        },
      },
    },
  },
})
vim.lsp.enable('ty')

vim.filetype.add({
  extension = {
    hlsl = 'shaderslang',
    hlsli = 'shaderslang',
  },
})
