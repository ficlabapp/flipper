#pragma once
#include "Interfaces/base.h"
#include "section.h"
#include "QScopedPointer"
#include "QSharedPointer"
#include "QSqlDatabase"
#include "QReadWriteLock"


namespace interfaces {
class Tags{
public:
    bool DeleteTag(QString);
    bool AddTag();
    QStringList ReadUserTags();
QSqlDatabase db;
private:
    QStringList CreateDefaultTagList();

    QSharedPointer<Fandoms> fandomInterface;

};


}
