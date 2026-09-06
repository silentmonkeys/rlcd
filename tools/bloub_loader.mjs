// bloub_loader.mjs —— 让 Node 22 的 --experimental-strip-types 能加载 bloub 源码
//
// bloub 的 src/bot/*.ts 全是无扩展名相对导入（'./math'），Node ESM 默认不解析；
// 这里挂一个 resolve hook：找不到时补 ".ts" 再试一次。配合
// --experimental-strip-types 即可零打包直接跑 bloub 的纯函数引擎。
export async function resolve(specifier, context, next) {
  try {
    return await next(specifier, context);
  } catch (e) {
    if (e?.code !== 'ERR_MODULE_NOT_FOUND') throw e;
    // 相对导入（'./math'）与绝对路径（BLOUB_ROOT 拼出来的）都补 ".ts" 再试
    if (specifier.startsWith('.') || specifier.startsWith('/')) {
      try {
        return await next(specifier + '.ts', context);
      } catch {
        // 落回原始错误
      }
    }
    throw e;
  }
}
