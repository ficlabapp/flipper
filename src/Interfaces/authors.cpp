#include "Interfaces/authors.h"
#include "Interfaces/db_interface.h"
#include "include/pure_sql.h"
#include <QSqlQuery>

namespace interfaces {
Authors::~Authors(){}
void Authors::Clear()
{
//    QList<QSharedPointer<core::Author>> authors;

//    QHash<QString, QSharedPointer<core::Author>> authorsByname;
//    QHash<QString, QHash<QString, QSharedPointer<core::Author>>> authorsByWebsite;
//    QHash<int, QSharedPointer<core::Author>> authorsByWebId;
//    QHash<int, QSharedPointer<core::Author>> authorsById;


//    QList<QSharedPointer<core::Author>> queue;
//    QHash<int, QSharedPointer<core::Fic>> queuedFicRecommendations;
    authors.clear();
    ClearIndex();
}

void Authors::Reindex()
{
    ClearIndex();
    IndexAuthors();
}

void Authors::IndexAuthors()
{
    for(auto author : authors)
    {
        authorsById[author->id] = author;
        authorsNamesByWebsite[author->website][author->name] = author;
        authorsByUrl[author->url("ffn")] = author;
    }
}

void Authors::ClearIndex()
{
    authorsNamesByWebsite.clear();
    authorsByUrl.clear();
    authorsById.clear();
}

QSharedPointer<core::Author> Authors::GetSingleByName(QString name, QString website)
{
    QSharedPointer<core::Author> result;
    if(authorsNamesByWebsite.contains(website) && authorsNamesByWebsite[website].contains(name))
        result = {authorsNamesByWebsite[website][name]};
    return result;

}

QList<QSharedPointer<core::Author>> Authors::GetAllByName(QString name)
{
    QList<QSharedPointer<core::Author>> result;

    for(auto websiteHash : authorsNamesByWebsite)
    {
        if(websiteHash.contains(name))
            result.push_back(websiteHash[name]);
    }

    return result;
}

QSharedPointer<core::Author> Authors::GetByUrl(QString url)
{
    QSharedPointer<core::Author> result;
    if(!authorsByUrl.contains(url))
        return result;
    return authorsByUrl[url];
}

QSharedPointer<core::Author > Authors::GetById(int id)
{
    QSharedPointer<core::Author> result;
    if(!authorsById.contains(id))
        return result;
    return authorsById[id];
}

QList<QSharedPointer<core::Author> > Authors::GetAllAuthors(QString website)
{
    if(website.isEmpty())
        return authors;
    if(!authorsNamesByWebsite.contains(website))
        return authors;
    return authorsNamesByWebsite[website].values();
}

QStringList Authors::GetAllAuthorsUrls(QString website)
{
    QStringList result;
    if(!cachedAuthorUrls.contains(website))
    {
        auto authors = GetAllAuthors(website);
        result.reserve(authors.size());
        for(auto author: authors)
        {
            result.push_back(author->url("ffn"));
        }
        cachedAuthorUrls[website] = result;
    }
    else
        result = cachedAuthorUrls[website];
    return result;
}

QList<int> Authors::GetAllAuthorIds()
{
    if(cachedAuthorIds.empty())
        cachedAuthorIds = database::puresql::GetAllAuthorIds(db);
    return cachedAuthorIds;
}

int Authors::GetFicCount(int authorId)
{
    auto author = GetById(authorId);
    if(!author)
        return 0;
    return author->ficCount;
}

int Authors::GetCountOfRecsForTag(int authorId, QString tag)
{
    auto result = database::puresql::GetCountOfTagInAuthorRecommendations(authorId, tag, db);
    return result;
}
bool Authors::LoadAuthors(QString website, bool additionMode)
{
    if(!additionMode)
        Clear();
    authors = database::puresql::GetAllAuthors(website, db);
    if(authors.size() == 0)
        return false;
    Reindex();
    return true;
}
bool Authors::EnsureId(QSharedPointer<core::Author> author, QString website)
{
    if(!author)
        return false;

    if(author->GetIdStatus() == core::AuthorIdStatus::unassigned)
        author->AssignId(database::puresql::GetAuthorIdFromUrl(author->url(website), db));
    if(author->GetIdStatus() == core::AuthorIdStatus::not_found)
    {
        database::puresql::WriteAuthor(author,portableDBInterface->GetCurrentDateTime(), db);
        author->AssignId(database::puresql::GetAuthorIdFromUrl(author->url(website), db));
    }
    if(author->id < 0)
        return false;
    AddAuthorToIndex(author);
    return true;
}

void Authors::AddAuthorToIndex(QSharedPointer<core::Author>)
{

}

bool Authors::AssignNewNameForAuthor(QSharedPointer<core::Author> author, QString name)
{
    if(!author)
        return false;
    return database::puresql::AssignNewNameForAuthor(author, name, db);
}


QSharedPointer<core::AuthorRecommendationStats> Authors::GetStatsForTag(int authorId, QSharedPointer<core::RecommendationList> list)
{
    QSharedPointer<core::AuthorRecommendationStats>result (new core::AuthorRecommendationStats);
    auto author = GetById(authorId);
    if(!author)
        return result;

    result->listId = list->id;
    result->usedTag = list->tagToUse;;
    result->authorName = author->name;
    result->authorId= author->id;
    result->totalFics= author->ficCount;

    result->matchesWithReference= database::puresql::GetCountOfTagInAuthorRecommendations(author->id, list->tagToUse, db);
    if(result->matchesWithReference == 0)
        result->matchRatio = 999999;
    else
        result->matchRatio = (double)result->totalFics/(double)result->matchesWithReference;
    result->isValid = true;

    return result;
}

bool  Authors::RemoveAuthor(int id)
{
    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q1(db);
    QString qsl = "delete from recommendations where recommender_id = %1";
    qsl=qsl.arg(QString::number(id));
    q1.prepare(qsl);
    if(!database::puresql::ExecAndCheck(q1))
        return false;

    QSqlQuery q2(db);
    qsl = "delete from recommenders where id = %1";
    qsl=qsl.arg(id);
    q2.prepare(qsl);
    if(!database::puresql::ExecAndCheck(q2))
        return false;
    return true;
}

bool Authors::RemoveAuthor(QSharedPointer<core::Author> author, QString website)
{
    int id = database::puresql::GetAuthorIdFromUrl(author->url(website),db);
    if(id == -1)
        return false;
    return RemoveAuthor(id);
}
}
