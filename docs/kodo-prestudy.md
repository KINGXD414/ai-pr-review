# 七牛云 KODO 集成预研

> 作者：love-June | 日期：2026-05-30

## 1. 概述

七牛云对象存储 KODO 是高可靠、强安全、低成本的存储服务，支持通过控制台、API、SDK 进行文件上传、下载和管理。本文档调研 C++ 场景下的集成方案，为 ai-pr-review 的 KODO 集成提供技术参考。

## 2. SDK 选项

### 2.1 七牛官方 C/C++ SDK

- 官方文档：[C/C++ SDK](https://developer.qiniu.com/kodo/sdk/cpp)
- 语言：纯 C 接口（C89 风格 struct + 函数），C++ 项目通过 extern "C" 调用
- 认证：AK/SK（AccessKey / SecretKey）签名
- 功能覆盖：
  - 文件上传（直传 / 分片上传）
  - 文件下载（公开 / 私有 URL）
  - 资源管理（stat / copy / move / delete / 批量操作）
  - 上传凭证（uptoken）生成

核心数据结构：

```c
typedef struct _Qiniu_Mac {
    const char* accessKey;
    const char* secretKey;
} Qiniu_Mac;

typedef struct _Qiniu_RS_PutPolicy {
    const char* scope;        // bucket 或 bucket:key
    const char* callbackUrl;  // 回调 URL（可选）
    const char* callbackBody; // 回调 body（可选）
    // ...更多字段
} Qiniu_RS_PutPolicy;

typedef struct _Qiniu_Error {
    int code;
    const char* message;
} Qiniu_Error;
```

上传示例：

```c
Qiniu_Mac mac;
mac.accessKey = "<ACCESS_KEY>";
mac.secretKey = "<SECRET_KEY>";

Qiniu_Client client;
Qiniu_Client_InitMacAuth(&client, 1024, &mac);
Qiniu_Client_EnableAutoQuery(&client, Qiniu_True);

// 生成上传凭证
Qiniu_RS_PutPolicy putPolicy;
Qiniu_Zero(putPolicy);
putPolicy.scope = "my-bucket";
char* token = Qiniu_RS_PutPolicy_Token(&putPolicy, &mac);

// 上传文件
Qiniu_Io_PutRet putRet;
Qiniu_Error err = Qiniu_Io_PutFile(&client, &putRet, token, "remote-key", "/path/to/local/file", NULL);
if (err.code != 200) {
    // 处理错误
}
Qiniu_Free(token);
Qiniu_Client_Cleanup(&client);
```

注意：`Qiniu_Error.message`、`Qiniu_RS_StatRet.hash` 等返回的 `const char*` 在下次 API 请求时失效，需用 `strdup()` 复制。

### 2.2 AWS SDK for C++（S3 协议兼容）

- 官方文档：[AWS SDK for C++ Examples](https://developer.qiniu.com/kodo/12554/aws-sdk-cpp-examples)
- 七牛 KODO 兼容 S3 协议，可用 AWS SDK for C++ 对接
- Endpoint：`https://s3.<region>.qiniucs.com`（如华东区 `s3.cn-east-1.qiniucs.com`）
- 认证：AK/SK 通过 `Aws::Auth::AWSCredentials` 传入

上传示例：

```cpp
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>

Aws::SDKOptions options;
Aws::InitAPI(options);
{
    const Aws::Auth::AWSCredentials cred("<QINIU_ACCESS_KEY>", "<QINIU_SECRET_KEY>");
    Aws::Client::ClientConfiguration config;
    config.endpointOverride = "https://s3.cn-east-1.qiniucs.com";
    config.region = "cn-east-1";

    Aws::S3::S3Client s3Client(cred,
        Aws::MakeShared<Aws::S3::S3EndpointProvider>(Aws::S3::S3Client::ALLOCATION_TAG),
        config);

    Aws::S3::Model::PutObjectRequest req;
    req.SetBucket("my-bucket");
    req.SetKey("review-report.md");
    auto body = Aws::MakeShared<Aws::FStream>("Upload", "/path/to/report.md",
                                               std::ios_base::in | std::ios_base::binary);
    req.SetBody(body);

    auto outcome = s3Client.PutObject(req);
    if (!outcome.IsSuccess()) {
        std::cerr << outcome.GetError().GetMessage() << std::endl;
    }
}
Aws::ShutdownAPI(options);
```

优势：
- 现代 C++ 接口，RAII 资源管理
- 支持分片上传、预签名 URL、生命周期管理
- 社区活跃，文档完善

劣势：
- 依赖体积大（AWS SDK 较重）
- 编译时间长

## 3. 认证方式

| 方式 | 说明 | 适用场景 |
|------|------|----------|
| AK/SK 签名 | 使用 AccessKey + SecretKey 对请求签名 | 服务端直传 |
| Upload Token (uptoken) | 服务端生成临时上传凭证，客户端凭此上传 | 客户端直传 |
| 预签名 URL | 生成带时效的临时访问 URL | 临时授权下载/上传 |

对于 ai-pr-review 场景（服务端 CLI 工具），推荐 AK/SK 签名方式，最简单直接。

## 4. 集成方案建议

### 方案 A：官方 C SDK（推荐）

- 集成简单：CMake `FetchContent` 拉取源码，无外部依赖
- 体积小：纯 C 实现，编译快
- 对齐项目风格：项目已有 C 风格 SDK 使用经验（libclang C API）

集成步骤：
1. CMake FetchContent 拉取七牛 C SDK
2. 新增 `code/src/storage/kodo_client.h/cpp`
3. 封装上传/下载接口，AK/SK 从环境变量读取
4. main.cpp 新增 `--upload-kodo <bucket>/<key>` flag
5. review 完成后自动上传报告到 KODO

### 方案 B：AWS SDK for C++

- 现代接口，但依赖重
- 适合后续如果需要 S3 生态兼容（如迁移到其他 S3 存储）

### 方案 C：HTTP 直调

- 用项目已有的 cpp-httplib 直接调七牛 REST API
- 无额外 SDK 依赖
- 需自行实现签名算法

**建议选方案 A**：官方 C SDK 最轻量，且项目已有类似集成经验。

## 5. 风险与注意事项

- AK/SK 安全：必须从环境变量读取，禁止硬编码或写入配置文件
- 上传凭证时效：uptoken 默认有效期 1 小时，长时间运行评测需刷新
- 存储区域：不同区域的 endpoint 不同，需配置
- 依赖冲突：官方 C SDK 使用 cJSON，可能与 nlohmann_json 冲突（不同库，无实际冲突）
- C SDK 维护状态：官方 C SDK 更新频率低于其他语言，需确认是否支持最新 API

## 6. 参考链接

- [C/C++ SDK 官方文档](https://developer.qiniu.com/kodo/sdk/cpp)
- [AWS SDK for C++ 示例](https://developer.qiniu.com/kodo/12554/aws-sdk-cpp-examples)
- [KODO 产品概述](https://developer.qiniu.com/kodo)
- [上传策略文档](https://developer.qiniu.com/kodo/1206/put-policy)
