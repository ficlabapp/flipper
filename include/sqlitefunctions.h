#pragma once
#include <QSqlQuery>
#include <QSqlDatabase>
#include "include/section.h"
#include "include/queryinterfaces.h"
#include "sqlite/sqlite3.h"


namespace database{

namespace sqlite {
int GetLastIdForTable(QString tableName, QSqlDatabase db);
void cfRegexp(sqlite3_context* ctx, int argc, sqlite3_value** argv);
void cfReturnCapture(sqlite3_context* ctx, int argc, sqlite3_value** argv);
void cfGetFirstFandom(sqlite3_context* ctx, int argc, sqlite3_value** argv);
void cfGetSecondFandom(sqlite3_context* ctx, int argc, sqlite3_value** argv);
bool InstallCustomFunctions(QSqlDatabase db);
bool ReadDbFile(QString file, QString connectionName);
QStringList GetIdListForQuery(QSharedPointer<core::Query> query, QSqlDatabase db);
bool BackupSqliteDatabase(QString dbname);
bool PushFandomToTopOfRecent(QString fandom, QSqlDatabase db);
QStringList FetchRecentFandoms(QSqlDatabase db);
bool RebaseFandomsToZero(QSqlDatabase db);
QDateTime GetCurrentDateTime(QSqlDatabase db);
QSqlDatabase InitDatabase(QString name, bool setDefault = false);
}

}