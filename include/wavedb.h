// WaveDB 统一公开头文件。
// 用户代码只需 #include "wavedb.h" 即可使用全部公开 API。
//
// 设计原则：
//   所有公开类型（Status/Value/Timestamp/Schema/Connection/Appender）在此聚合，
//   内部类型（Catalog/Part/ColumnFile/PartManager）对用户不可见，
//   由 Connection 和 Appender 提供全部读写能力。
#pragma once
#include "wavedb/appender.h"
#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/schema.h"
#include "wavedb/status.h"
#include "wavedb/types.h"
