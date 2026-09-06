// bloub_register.mjs —— 注册 resolve hook（供 --import 使用）
import { register } from 'node:module';
register('./bloub_loader.mjs', import.meta.url);
