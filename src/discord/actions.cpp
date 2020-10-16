#include "discord/actions.h"
#include "discord/command_generators.h"
#include "discord/discord_init.h"
#include "discord/help_generator.h"
#include "discord/db_vendor.h"
#include "sql/discord/discord_queries.h"
#include "discord/discord_server.h"
#include "discord/fetch_filters.h"
#include "discord/favourites_fetching.h"
#include "discord/type_strings.h"
#include "parsers/ffn/favparser_wrapper.h"
#include "Interfaces/interface_sqlite.h"
#include "Interfaces/fandoms.h"
#include "Interfaces/discord/users.h"
#include "grpc/grpc_source.h"
#include "timeutils.h"
#include <QUuid>
#include <QRegExp>
#include <QSettings>
namespace discord {

QSharedPointer<SendMessageCommand> ActionBase::Execute(QSharedPointer<TaskEnvironment> environment, Command command)
{
    action = SendMessageCommand::Create();
    action->originalMessage = command.originalMessage;
    action->user = command.user;
    action->originalCommandType = command.type;
    if(command.type != ECommandType::ct_timeout_active)
        command.user->initNewEasyQuery();
    environment->ficSource->SetUserToken(command.user->GetUuid());
    return ExecuteImpl(environment, command);

}

template<typename T> QString GetHelpForCommandIfActive(){
    QString result;
    if(CommandState<T>::active)
        result = "\n" + QString::fromStdString(std::string(TypeStringHolder<T>::help));
    return result;
}

QSharedPointer<SendMessageCommand> HelpAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    QString helpString;
    helpString +=  GetHelpForCommandIfActive<RecsCreationCommand>();
    helpString +=  GetHelpForCommandIfActive<NextPageCommand>();
    helpString +=  GetHelpForCommandIfActive<PreviousPageCommand>();
    helpString +=  GetHelpForCommandIfActive<PageChangeCommand>();
    helpString +=  GetHelpForCommandIfActive<SetFandomCommand>();
    helpString +=  GetHelpForCommandIfActive<IgnoreFandomCommand>();
    helpString +=  GetHelpForCommandIfActive<IgnoreFicCommand>();
    helpString +=  GetHelpForCommandIfActive<DisplayHelpCommand>();
    helpString +=  GetHelpForCommandIfActive<RngCommand>();
    helpString +=  GetHelpForCommandIfActive<FilterLikedAuthorsCommand>();
    helpString +=  GetHelpForCommandIfActive<ShowFreshRecsCommand>();
    helpString +=  GetHelpForCommandIfActive<ShowCompletedCommand>();
    helpString +=  GetHelpForCommandIfActive<HideDeadCommand>();
    helpString +=  GetHelpForCommandIfActive<SimilarFicsCommand>();
    helpString +=  GetHelpForCommandIfActive<ResetFiltersCommand>();
    helpString +=  GetHelpForCommandIfActive<ChangeServerPrefixCommand>();
    helpString +=  GetHelpForCommandIfActive<PurgeCommand>();


    //"\n!status to display the status of your recommentation list"
    //"\n!status fandom/fic X displays the status for fandom or a fic (liked, ignored)"
    action->text = helpString.arg(QString::fromStdString(command.server->GetCommandPrefix()));
    return action;
}


QSharedPointer<SendMessageCommand> GeneralHelpAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    auto prefix = command.server->GetCommandPrefix();
    auto embed = GetHelpPage(command.ids.at(0), command.server->GetCommandPrefix());
    action->embed = embed;
    command.user->SetCurrentHelpPage(command.ids.at(0));
    if(command.targetMessage.string().length() > 0)
        action->targetMessage = command.targetMessage;
    action->reactionsToAdd.push_back("%f0%9f%91%88");
    action->reactionsToAdd.push_back("%f0%9f%91%89");

    return action;
}


QSharedPointer<core::RecommendationList> CreateRecommendationParams(QString ffnId)
{
    QSharedPointer<core::RecommendationList> list(new core::RecommendationList);
    list->minimumMatch = 1;
    list->maxUnmatchedPerMatch = 50;
    list->isAutomatic = true;
    list->useWeighting = true;
    list->alwaysPickAt = 9999;
    list->useMoodAdjustment = true;
    list->name = "Recommendations";
    list->assignLikedToSources = true;
    list->userFFNId = ffnId.toInt();
    return list;
}


QSharedPointer<core::RecommendationList> CreateSimilarFicParams()
{
    QSharedPointer<core::RecommendationList> list(new core::RecommendationList);
    list->minimumMatch = 1;
    list->maxUnmatchedPerMatch = 5000;
    list->isAutomatic = false;
    list->useWeighting = false;
    list->alwaysPickAt = 9999;
    list->useMoodAdjustment = false;
    list->name = "Recommendations";
    list->assignLikedToSources = true;
    list->userFFNId = -1;
    return list;
}

QSharedPointer<core::RecommendationList> FillUserRecommendationsFromFavourites(QString ffnId, QSet<QString> userFavourites, QSharedPointer<TaskEnvironment> environment, Command command){
    auto recList = CreateRecommendationParams(ffnId);
    recList->ignoreBreakdowns= true;

    QVector<core::Identity> pack;
    pack.resize(userFavourites.size());
    int i = 0;
    for(const auto& source: userFavourites)
    {
        pack[i].web.ffn = source.toInt();
        i++;
    }

    environment->ficSource->ClearUserData();
    environment->ficSource->GetInternalIDsForFics(&pack);
    UserData userData;

    auto ignoredFandoms =  command.user->GetCurrentIgnoredFandoms();
    for(auto& token: ignoredFandoms.tokens)
        recList->ignoredFandoms.insert(token.id);

    for(const auto& source: std::as_const(pack))
    {
        recList->ficData->sourceFics+=source.id;
        recList->ficData->fics+=source.web.ffn;
    }

    environment->ficSource->userData = userData;
    environment->ficSource->GetRecommendationListFromServer(recList);
    //QLOG_INFO() << "Got fics";
    // refreshing the currently saved recommendation list params for user
    An<interfaces::Users> usersDbInterface;
    usersDbInterface->DeleteUserList(command.user->UserID(), "");
    usersDbInterface->WriteUserList(command.user->UserID(), "", discord::elt_favourites, recList->minimumMatch, recList->maxUnmatchedPerMatch, recList->alwaysPickAt);
    usersDbInterface->WriteUserFFNId(command.user->UserID(), command.ids.at(0));

    // instantiating working set for user
    An<Users> users;
    command.user->SetFicList(recList->ficData);
    //QMap<int, int> scoreStatus; // maps maptch count to fic count with this match
    QMap<int, QSet<int>> matchFicToScore; // maps maptch count to fic count with this match
    int count = 0;
    if(!recList->ficData->matchCounts.size())
        return recList;

    for(int i = 0; i < recList->ficData->fics.size(); i++){
        if(recList->ficData->matchCounts.at(i) > 1)
        {
            matchFicToScore[recList->ficData->matchCounts.at(i)].insert(recList->ficData->fics.at(i));
            count++;
        }
    }
    int perfectRange = count*0.05; // 5% of the list with score > 1
    int goodRange = count*.15; // 15% of the list with score > 1
    int perfectCutoff = 0, goodCutoff = 0;
    auto keys = matchFicToScore.keys();
    std::sort(keys.begin(), keys.end());
    std::reverse(keys.begin(), keys.end());
    int currentFront = 0;
    QSet<int> perfectRngFics, goodRngFics;
    for(auto key : keys){
        currentFront += matchFicToScore[key].size();
        if(currentFront > perfectRange && perfectCutoff == 0)
            perfectCutoff = key;
        else if(currentFront  < perfectRange){
            perfectRngFics += matchFicToScore[key];
        }
        goodRngFics += matchFicToScore[key];
        if(currentFront > goodRange && goodCutoff == 0)
        {
            goodCutoff = key;
            break;
        }
    }
    if(goodCutoff == 0)
        goodCutoff=2;

    QLOG_INFO() << "Total fic count: " << count << " Perfect Fics: " << perfectRngFics.size() << " Good Fics: " << goodRngFics.size();

    command.user->SetFfnID(ffnId);
    //command.user->SetPerfectRngFics(perfectRngFics);
    //command.user->SetGoodRngFics(goodRngFics);
    command.user->SetPerfectRngScoreCutoff(perfectCutoff);
    command.user->SetGoodRngScoreCutoff(goodCutoff);
    environment->ficSource->ClearUserData();
    return recList;
}






QSharedPointer<SendMessageCommand> MobileRecsCreationAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    command.user->initNewRecsQuery();
    auto ffnId = QString::number(command.ids.at(0));
    bool refreshing = command.variantHash.contains("refresh");
    QSharedPointer<core::RecommendationList> listParams;
    //QString error;

    FavouritesFetchResult userFavourites = FetchMobileFavourites(ffnId, refreshing ? ECacheMode::use_only_cache : ECacheMode::dont_use_cache);
    // here, we check that we were able to fetch favourites at all
    if(!userFavourites.hasFavourites || userFavourites.errors.size() > 0)
    {
        action->text = userFavourites.errors.join("\n");
        action->stopChain = true;
        return action;
    }
    // here, we check that we were able to fetch all favourites with desktop link and reschedule the task otherwise
    if(userFavourites.requiresFullParse)
    {
        action->text = "Your favourite list is bigger than 500 favourites, sending it to secondary parser. You will be pinged when the recommendations are ready.";
        return action;
    }
    bool wasAutomatic = command.user->GetForcedMinMatch() == 0;
    auto recList = FillUserRecommendationsFromFavourites(ffnId, userFavourites.links, environment, command);
    if(wasAutomatic && !recList->isAutomatic)
    {
//        command.user->SetForcedMinMatch(recList->minimumMatch);
//        command.user->SetForcedRatio(recList->maxUnmatchedPerMatch);
        auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
        environment->fandoms->db = dbToken->db;
        An<interfaces::Users> usersDbInterface;
        usersDbInterface->WriteForcedListParams(command.user->UserID(), recList->minimumMatch,recList->maxUnmatchedPerMatch);
    }

    //qDebug() << "after filling";

    if(!recList->ficData->matchCounts.size())
    {
        command.user->SetFfnID(ffnId);
        action->text = "Couldn't create recommendations. Recommendations server is not available or you don't have any favourites on your ffn page. If it isn't the latter case, you can ping the author: zekses#3495";
        action->stopChain = true;
        return action;
    }

    if(!refreshing)
        action->text = "Recommendation list has been created for FFN ID: " + QString::number(command.ids.at(0));
    environment->ficSource->ClearUserData();
    command.user->SetRngBustScheduled(true);
    //qDebug() << "after clearing user data";
    return action;
}

static std::string CreateMention(const std::string& string){
    return "<@" + string + ">";
}

QSharedPointer<SendMessageCommand> DesktopRecsCreationAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    command.user->initNewRecsQuery();

    QString ffnId;
    bool isId = true;
    if(!command.variantHash.contains("url")){
        ffnId = QString::number(command.ids.at(0));
        isId = true;
    }
    else{
        isId = false;
        ffnId = command.variantHash["url"].toString();
    }

    bool refreshing = command.variantHash.contains("refresh");
    QSharedPointer<core::RecommendationList> listParams;
    //QString error;

    FavouritesFetchResult userFavourites = TryFetchingDesktopFavourites(ffnId, refreshing ? ECacheMode::use_only_cache : ECacheMode::dont_use_cache, isId);
    ffnId = userFavourites.ffnId;
    command.ids.clear();
    command.ids.push_back(userFavourites.ffnId.toUInt());


    // here, we check that we were able to fetch favourites at all
    if(!userFavourites.hasFavourites || userFavourites.errors.size() > 0)
    {
        action->text = userFavourites.errors.join("\n");
        action->stopChain = true;
        return action;
    }
    // here, we check that we were able to fetch all favourites with desktop link and reschedule the task otherwise
    if(userFavourites.requiresFullParse)
    {
        action->stopChain = true;
        action->text = QString::fromStdString(CreateMention(command.user->UserID().toStdString()) + " Your favourite list is bigger than 500 favourites, sending it to secondary parser. You will be pinged when the recommendations are ready.");
        command.type = ct_create_recs_from_mobile_page;
        command.variantHash = command.variantHash;
        Command displayRecs = NewCommand(command.server, command.originalMessage, ct_display_page);
        displayRecs.variantHash["refresh_previous"] = true;
        displayRecs.user = command.user;
        displayRecs.ids.push_back(0);
        CommandChain chain;
        chain.commands += command;
        chain.commands += displayRecs;
        chain.hasFullParseCommand = true;
        chain.user = command.user;
        action->commandsToReemit.push_back(chain);
        return action;
    }
    bool wasAutomatic = command.user->GetForcedMinMatch() == 0;
    auto recList = FillUserRecommendationsFromFavourites(ffnId, userFavourites.links, environment,command);
    if(wasAutomatic && !recList->isAutomatic)
    {
        command.user->SetForcedMinMatch(recList->minimumMatch);
        command.user->SetForcedRatio(recList->maxUnmatchedPerMatch);
        auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
        environment->fandoms->db = dbToken->db;
        An<interfaces::Users> usersDbInterface;
        usersDbInterface->WriteForcedListParams(command.user->UserID(), recList->minimumMatch,recList->maxUnmatchedPerMatch);
    }


    if(!recList->ficData->matchCounts.size())
    {
        command.user->SetFfnID(ffnId);
        action->text = QString::fromStdString(CreateMention(command.user->UserID().toStdString()) + " Couldn't create recommendations. Recommendations server is not available or you don't have any favourites on your ffn page. If it isn't the latter case, you can ping the author: zekses#3495");
        action->stopChain = true;
        return action;
    }

    if(!refreshing)
        action->text = "Recommendation list has been created for FFN ID: " + QString::number(command.ids.at(0));
    command.user->SetRngBustScheduled(true);
    environment->ficSource->ClearUserData();
    return action;
}



void ActionChain::Push(QSharedPointer<SendMessageCommand> action)
{
    actions.push_back(action);
}

QSharedPointer<SendMessageCommand> ActionChain::Pop()
{
    auto action = actions.first();
    actions.pop_front();
    return action;
}


auto ExtractAge(QDateTime toDate){
    int years = 0;
    int months = 0;
    int days = 0;
    QDate beginDate= toDate.date();
    QDate endDate= QDateTime::currentDateTimeUtc().date();
    if(beginDate.daysTo(endDate) >= 0)
    {
        years=endDate.year()-beginDate.year();
        if((months=endDate.month()-beginDate.month())<0)
        {
            years--;
            months+=12;
        }
        if((days=endDate.day()-beginDate.day())<0)
        {
            if(--months < 0)
            {
                years--;
                months+=12;
            }
            days+=beginDate.daysInMonth();
        }
      }

    QStringList dates;
    if(years > 0)
        dates += QString::number(years) + "y";
    if(months > 0)
        dates += QString::number(months) + "m";
    if(days > 0)
        dates += QString::number(days) + "d";
    return dates;
}

void  FillListEmbedForFicAsFields(SleepyDiscord::Embed& embed, core::Fanfic& fic, int i, bool addNewlines = true){
    QString urlProto = "[%1](https://www.fanfiction.net/s/%2)";
    //QString authorUrlProto = "[%1](https://www.fanfiction.net/u/%2)";
    auto fandomsList=fic.fandoms;
    SleepyDiscord::EmbedField field;
    field.isInline = true;
    field.name = QString("Fandom: `" + fandomsList.join(" & ").replace("'", "\\'") + "`").rightJustified(20, ' ').toStdString();
    field.value += QString("ID: " + QString::number(i)).rightJustified(2, ' ').toStdString();
    field.value += QString(" " + urlProto.arg(fic.title, QString::number(fic.identity.web.GetPrimaryId()))+"\n").toStdString();
    //field.value += QString("Fandom: `" + fandomsList.join(" & ").replace("'", "\\'") + "`").rightJustified(20, ' ').toStdString();
    //embed.description += " by: "  + QString(" " + authorUrlProto.arg(fic.author).arg(QString::number(fic.author_id))+"\n").toStdString();
    field.value += QString("Length: `" + fic.wordCount + "`").toStdString();
    field.value += QString("\nScore: `" + QString::number(fic.score) + "`").toStdString();
    //field.value += QString("n").toHtmlEscaped().toStdString();
    QString genre = fic.statistics.realGenreString.split(",").join("/").replace(QRegExp("#c#"),"+").replace(QRegExp("#p#"),"=").replace(QRegExp("#b#"),"~");
    if(genre.isEmpty())
        genre = fic.genreString;

    field.value  += QString("\nGenre: `" + genre + "`").toStdString();
    if(fic.complete)
        field.value  += QString("\nComplete").rightJustified(12).toStdString();
    else
    {
        QDateTime date = fic.updated.isValid() && fic.updated.date().year() != 1970 ? fic.updated : fic.published;
        auto dates = ExtractAge(date);
        if(dates.size())
            field.value  += ("\nIncomplete: `" + dates.join(" ") + "`").rightJustified(12).toStdString();
        else
            field.value  += QString("\nIncomplete:`").rightJustified(12).toStdString();
    }

    if(addNewlines)
        field.value  += "\n\n";
    auto temp =  QString::fromStdString(field.value);
    temp = temp.replace("````", "```");
    field.value  = temp.toStdString();
    embed.fields.push_back(field);
}


void  FillListEmbedForFic(SleepyDiscord::Embed& embed, core::Fanfic& fic, int i, bool addNewlines = true){
    QString urlProto = "[%1](https://www.fanfiction.net/s/%2)";
       //QString authorUrlProto = "[%1](https://www.fanfiction.net/u/%2)";
       auto fandomsList=fic.fandoms;
       embed.description += QString("ID: " + QString::number(i)).rightJustified(2, ' ').toStdString();
       embed.description += QString(" " + urlProto.arg(fic.title, QString::number(fic.identity.web.GetPrimaryId()))+"\n").toStdString();
       embed.description += QString("Fandom: `" + fandomsList.join(" & ").replace("'", "\\'") + "`").rightJustified(20, ' ').toStdString();
       //embed.description += " by: "  + QString(" " + authorUrlProto.arg(fic.author).arg(QString::number(fic.author_id))+"\n").toStdString();
       embed.description += QString("\nLength: `" + fic.wordCount + "`").toStdString();
       embed.description += QString(" Score: `" + QString::number(fic.score) + "`").toStdString();
       embed.description += QString(" Status:  ").toHtmlEscaped().toStdString();
       if(fic.complete)
           embed.description += QString(" `Complete`  ").rightJustified(12).toStdString();
       else
       {
           QDateTime date = fic.updated.isValid() && fic.updated.date().year() != 1970 ? fic.updated : fic.published;
           auto dates = ExtractAge(date);
           if(dates.size())
               embed.description += (" `Incomplete: " + dates.join(" ") + "`").rightJustified(12).toStdString();
           else
               embed.description += QString(" `Incomplete`").rightJustified(12).toStdString();
       }
       QString genre = fic.statistics.realGenreString.split(",").join("/").replace(QRegExp("#c#"),"+").replace(QRegExp("#p#"),"=").replace(QRegExp("#b#"),"~");
       if(genre.isEmpty())
           genre = fic.genreString;

       embed.description += QString("\nGenre: `" + genre + "`").toStdString();
       if(addNewlines)
           embed.description += "\n\n";
       auto temp =  QString::fromStdString(embed.description);
       temp = temp.replace("````", "```");
       embed.description = temp.toStdString();
}



void  FillDetailedEmbedForFic(SleepyDiscord::Embed& embed, core::Fanfic& fic, int i, bool asFields){
    if(asFields)
        FillListEmbedForFicAsFields(embed, fic, i);
    else{
        FillListEmbedForFic(embed, fic, i, false);
        embed.description += (QString("\n```") + fic.summary + QString("```")).toStdString();
        embed.description += "\n";
        auto temp =  QString::fromStdString(embed.description);
        temp = temp.replace("````", "```");
        //temp = temp.replace("'", "\'");
        embed.description = temp.toStdString();
    }
}




void  FillActiveFilterPartInEmbed(SleepyDiscord::Embed& embed, QSharedPointer<TaskEnvironment> environment, Command& command){
    auto filter = command.user->GetCurrentFandomFilter();

    QString result;
    if(command.user->GetSimilarFicsId() != 0)
        result += QString("\nDisplaying similarity list for fic: %1.").arg(command.user->GetSimilarFicsId());
    if(filter.fandoms.size() > 0){
        result += "\nDisplayed recommendations are for fandom filter:\n";
        for(auto fandom: std::as_const(filter.fandoms))
        {
            if(fandom != -1)
                result += ( " - " + environment->fandoms->GetNameForID(fandom) + "\n");
        }
    }

    auto wordcountFilter = command.user->GetWordcountFilter();
    if(wordcountFilter.firstLimit !=0 && wordcountFilter.secondLimit/1000 == 99999999)
        result += QString("\nShowing fics with > %1k words.").arg(QString::number(wordcountFilter.firstLimit/1000));
    if(wordcountFilter.firstLimit == 0 && wordcountFilter.secondLimit != 0)
        result += QString("\nShowing fics with < %1k words.").arg(QString::number(wordcountFilter.secondLimit/1000));
    if(wordcountFilter.firstLimit != 0 && wordcountFilter.secondLimit != 0  && wordcountFilter.secondLimit/1000 != 99999999)
        result += QString("\nShowing fics between %1k and %2k words.").arg(QString::number(wordcountFilter.firstLimit/1000),QString::number(wordcountFilter.secondLimit/1000));


    if(command.user->GetLastPageType() == ct_display_rng)
        result += QString("\nRolling in range: %1.").arg(command.user->GetLastUsedRoll());
    if(command.user->GetUseLikedAuthorsOnly())
        result += "\nLiked authors filter is active.";
    if(command.user->GetSortFreshFirst())
        result += "\nFresh recommendations sorting is active.";
    if(command.user->GetShowCompleteOnly())
        result += "\nOnly showing fics that are complete.";
    if(command.user->GetHideDead())
        result += "\nOnly showing fics that are not dead.";
    if(!result.isEmpty())
    {
        QString temp = "\nTo disable any active filters, repeat the command that activates them,\nor issue %2xfilter to remove them all.";
        temp = temp.arg(QString::fromStdString(command.server->GetCommandPrefix()));
        result += temp;
    }

    embed.description += result.toStdString();
}

void  FillActiveFilterPartInEmbedAsField(SleepyDiscord::Embed& embed, QSharedPointer<TaskEnvironment> environment, Command& command){
    auto filter = command.user->GetCurrentFandomFilter();
    SleepyDiscord::EmbedField field;
    field.isInline = false;
    field.name = "Active filters:";
    if(filter.fandoms.size() > 0){
        field.value += "\nDisplayed recommendations are for fandom filter:\n";
        for(auto fandom: std::as_const(filter.fandoms))
        {
            if(fandom != -1)
                field.value += ( " - " + environment->fandoms->GetNameForID(fandom) + "\n").toStdString();
        }
    }
    if(command.user->GetUseLikedAuthorsOnly())
        field.value += "\nLiked authors filter is active.";
    if(command.user->GetSortFreshFirst())
        field.value += "\nFresh recommendations sorting is active.";
    embed.fields.push_back(field);
}

QSharedPointer<SendMessageCommand> DisplayPageAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    QLOG_TRACE() << "Creating page results";

    environment->ficSource->ClearUserData();
    auto page = command.ids.at(0);
    command.user->SetPage(page);
    An<interfaces::Users> usersDbInterface;
    usersDbInterface->UpdateCurrentPage(command.user->UserID(), page);

    QVector<core::Fanfic> fics;
    QLOG_TRACE() << "Fetching fics";
    environment->ficSource->ClearUserData();
    FetchFicsForDisplayPageCommand(environment->ficSource, command.user, 9, &fics);
    int pageCount = FetchPageCountForFilterCommand(environment->ficSource, command.user, 9);
    auto userFics = command.user->FicList();
    for(auto& fic : fics)
        fic.score = userFics->ficToScore[fic.identity.id];
    QLOG_TRACE() << "Fetched fics";
    SleepyDiscord::Embed embed;
    //QString urlProto = "[%1](https://www.fanfiction.net/s/%2)";
    //QString authorUrlProto = "[%1](https://www.fanfiction.net/u/%2)";
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    environment->fandoms->FetchFandomsForFics(&fics);
    auto editPreviousPageIfPossible = command.variantHash["refresh_previous"].toBool();

    if(command.targetMessage.string().length() != 0){
        action->text = QString::fromStdString(CreateMention(command.user->UserID().toStdString()) + ", here are the results:");
    }
    else{
        auto previousPage = command.user->GetLastPageMessage();
        if(editPreviousPageIfPossible && previousPage.message.string().length() > 0 && previousPage.channel == command.originalMessage.channelID)
        {
            action->text = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", here are the results:");
            action->diagnosticText = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", your previous results have been updated with new data." );
            action->targetMessage = previousPage.message;
        }
        else
            action->text = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", here are the results:");
        //        action->text = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", here are the results:");
    }

    embed.description = QString("Generated recs for user [%1](https://www.fanfiction.net/u/%1), page: %2 of %3").arg(command.user->FfnID()).arg(command.user->CurrentRecommendationsPage()).arg(QString::number(pageCount)).toStdString();
    auto& tips = SendMessageCommand::tips;
    int tipNumber =  rand() % tips.size();
    bool showAppOrPatreon = rand() % 7 == 0;
    SleepyDiscord::EmbedFooter footer;
    if(showAppOrPatreon){
        QStringList appPromo =  {"Socrates has a PC desktop app version called Flipper that has more filters and is more convenient to use. You can get it at https://github.com/Zeks/flipper/releases/latest",
                                 "If you would like to support the bot, you can do it on https://www.patreon.com/Zekses"};
        int shownId = rand() %2 == 0;
        footer.text = appPromo.at(shownId).toStdString();
        if(shownId == 1)
            footer.iconUrl = "https://c5.patreon.com/external/logo/downloads_logomark_color_on_white@2x.png";
        else
            footer.iconUrl = "https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png";
    }
    else{
        if(tips.size() > 0)
        {
            auto temp = tips.at(tipNumber);
            if(temp.contains("%1"))
                temp =temp.arg(QString::fromStdString(command.server->GetCommandPrefix()));
            footer.text = temp .toStdString();
        }
    }
    embed.footer = footer;

    FillActiveFilterPartInEmbed(embed, environment, command);

    QHash<int, int> positionToId;
    int i = 0;
    for(auto fic: std::as_const(fics))
    {
        positionToId[i+1] = fic.identity.id;
        i++;
        FillListEmbedForFicAsFields(embed, fic, i);
    }

    command.user->SetPositionsToIdsForCurrentPage(positionToId);
    command.user->SetLastPageType(ct_display_page);
    action->embed = embed;
    action->reactionsToAdd.push_back("%f0%9f%91%88");
    action->reactionsToAdd.push_back("%f0%9f%91%89");
    if(command.targetMessage.string().length() > 0)
        action->targetMessage = command.targetMessage;
    environment->ficSource->ClearUserData();
    QLOG_INFO() << "Created page results";
    return action;
}


QSharedPointer<SendMessageCommand> DisplayRngAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto quality = command.variantHash["quality"].toString().trimmed();
    if(quality.length() == 0)
        quality = command.user->GetLastUsedRoll().isEmpty() ? "all " : command.user->GetLastUsedRoll();
    QVector<core::Fanfic> fics;
    command.user->SetLastUsedRoll(quality);

    //QSet<int> filteringFicSet;
    int scoreCutoff = 1;
    if(quality == "best")    {
        scoreCutoff= command.user->GetPerfectRngScoreCutoff();
    }
    if(quality == "good"){
        scoreCutoff= command.user->GetGoodRngScoreCutoff();
    }

    QLOG_TRACE() << "Fetching fics for rng";
    FetchFicsForDisplayRngCommand(3, environment->ficSource, command.user, &fics, scoreCutoff);
    auto userFics = command.user->FicList();
    for(auto& fic : fics)
        fic.score = userFics->ficToScore[fic.identity.id];

    QLOG_TRACE() << "Fetched fics for rng";

    // fetching fandoms for selected fics
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    environment->fandoms->FetchFandomsForFics(&fics);

    SleepyDiscord::Embed embed;
    //QString urlProto = "[%1](https://www.fanfiction.net/s/%2)";
    auto editPreviousPageIfPossible = command.variantHash["refresh_previous"].toBool();
    if(command.targetMessage.string().length() != 0)
        action->text = QString::fromStdString(CreateMention(command.user->UserID().toStdString()) + ", here are the results:");
    else {
        auto previousPage = command.user->GetLastPageMessage();
        if(editPreviousPageIfPossible && previousPage.message.string().length() > 0 && previousPage.channel == command.originalMessage.channelID)
        {
            action->text = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", here are the results:");
            action->diagnosticText = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", your previous results have been updated with new data." );
            action->targetMessage = previousPage.message;
        }
        else
            action->text = QString::fromStdString(CreateMention(command.originalMessage.author.ID.string()) + ", here are the results:");
    }


    QHash<int, int> positionToId;
    int i = 0;
    for(auto fic: std::as_const(fics))
    {
        positionToId[i+1] = fic.identity.id;
        i++;
        FillDetailedEmbedForFic(embed, fic, i,false);
    }

    command.user->SetPositionsToIdsForCurrentPage(positionToId);
    command.user->SetLastPageType(ct_display_rng);
    FillActiveFilterPartInEmbed(embed, environment, command);

    action->embed = embed;
    action->reactionsToAdd.push_back("%f0%9f%94%81");
    if(command.targetMessage.string().length() > 0)
        action->targetMessage = command.targetMessage;
    QLOG_INFO() << "Created page results";
    return action;


}


QSharedPointer<SendMessageCommand> SetFandomAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto fandom = command.variantHash["fandom"].toString().trimmed();
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    auto fandomId = environment->fandoms->GetIDForName(fandom);
    auto currentFilter = command.user->GetCurrentFandomFilter();
    An<interfaces::Users> usersDbInterface;
    if(command.variantHash.contains("reset"))
    {
        usersDbInterface->ResetFandomFilter(command.user->UserID());
        command.user->ResetFandomFilter();
        action->emptyAction = true;
        return action;
    }
    if(fandomId == -1)
    {
        action->text = "`" + fandom  + "`" + " is not a valid fandom";
        action->stopChain = true;
        return action;
    }
    if(currentFilter.fandoms.contains(fandomId))
    {
        currentFilter.RemoveFandom(fandomId);
        action->text = "Removing filtered fandom: " + fandom;
        usersDbInterface->UnfilterFandom(command.user->UserID(), fandomId);
    }
    else if(currentFilter.fandoms.size() == 2)
    {
        auto oldFandomId = currentFilter.tokens.last().id;
        action->text = "Replacing crossover fandom: " + environment->fandoms->GetNameForID(currentFilter.tokens.last().id) +    " with: " + fandom;
        usersDbInterface->UnfilterFandom(command.user->UserID(), oldFandomId);
        usersDbInterface->FilterFandom(command.user->UserID(), fandomId, command.variantHash["allow_crossovers"].toBool());
        currentFilter.RemoveFandom(oldFandomId);
        currentFilter.AddFandom(fandomId, command.variantHash["allow_crossovers"].toBool());
        return action;
    }
    else {
        usersDbInterface->FilterFandom(command.user->UserID(), fandomId, command.variantHash["allow_crossovers"].toBool());
        currentFilter.AddFandom(fandomId, command.variantHash["allow_crossovers"].toBool());
    }
    command.user->SetRngBustScheduled(true);
    command.user->SetFandomFilter(currentFilter);
    action->emptyAction = true;
    return action;
}

QSharedPointer<SendMessageCommand> IgnoreFandomAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto fandom = command.variantHash["fandom"].toString().trimmed();
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    auto fandomId = environment->fandoms->GetIDForName(fandom);
    auto properNameForFandom = environment->fandoms->GetNameForID(fandomId);

    auto withCrossovers = command.variantHash["with_crossovers"].toBool();
    An<interfaces::Users> usersDbInterface;
    if(command.variantHash.contains("reset"))
    {
        usersDbInterface->ResetFandomIgnores(command.user->UserID());
        command.user->ResetFandomIgnores();
        action->emptyAction = true;
        return action;
    }
    if(fandomId == -1)
    {
        action->text = "`" + fandom  + "`" + " is not a valid fandom";
        action->stopChain = true;
        return action;
    }

    auto currentIgnores = command.user->GetCurrentIgnoredFandoms();
    auto ignoreFandom = [&](){
        usersDbInterface->IgnoreFandom(command.user->UserID(), fandomId, withCrossovers);
        currentIgnores.AddFandom(fandomId, withCrossovers);
        action->text = "Adding fandom: " + properNameForFandom + " to ignores";
        if(withCrossovers)
            action->text += ".Will also exclude crossovers from now on.";
        else
            action->text += ".";
    };

    if(currentIgnores.fandoms.contains(fandomId)){
        auto token = currentIgnores.GetToken(fandomId);
        if(!token.includeCrossovers && withCrossovers)
        {
            currentIgnores.RemoveFandom(fandomId);
            ignoreFandom();
        }
        else{
            usersDbInterface->UnignoreFandom(command.user->UserID(), fandomId);
            currentIgnores.RemoveFandom(fandomId);
            action->text = "Removing fandom: " + properNameForFandom + " from ignores";
        }
    }
    else{
        ignoreFandom();

    }
    command.user->SetRngBustScheduled(true);
    command.user->SetIgnoredFandoms(currentIgnores);
    //action->emptyAction = true;
    return action;
}

QSharedPointer<SendMessageCommand> IgnoreFicAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    auto ficIds = command.ids;
    if(command.variantHash.contains("everything"))
        ficIds = {1,2,3,4,5,6,7,8,9,10};

    auto ignoredFics = command.user->GetIgnoredFics();
    An<interfaces::Users> usersDbInterface;
    QStringList ignoredIds;

    for(auto positionalId : std::as_const(ficIds)){
        //need to get ffn id from positional id
        auto ficId = command.user->GetFicIDFromPositionId(positionalId);
        if(ficId != -1){
            if(!ignoredFics.contains(ficId))
            {
                ignoredFics.insert(ficId);
                usersDbInterface->TagFanfic(command.user->UserID(), "ignored",  ficId);
                ignoredIds.push_back(QString::number(positionalId));
            }
            else{
                ignoredFics.remove(ficId);
                usersDbInterface->UnTagFanfic(command.user->UserID(), "ignored",  ficId);
            }
        }
    }
    command.user->SetIgnoredFics(ignoredFics);
    action->text = "Ignored fics: " + ignoredIds.join(" ");
    return action;
}

QSharedPointer<SendMessageCommand> TimeoutActiveAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    auto reason = command.variantHash["reason"].toString();
    auto seconds= command.ids.at(0);
    action->text = reason .arg(QString::number(seconds));
    return action;
}

QSharedPointer<SendMessageCommand> NoUserInformationAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    action->text = QString("You need to call %1recs FFN_ID first").arg(QString::fromStdString(command.server->GetCommandPrefix()));
    return action;
}

QSharedPointer<SendMessageCommand> ChangePrefixAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    if(command.variantHash.contains("prefix")){
        auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
        command.server->SetCommandPrefix(command.variantHash["prefix"].toString().trimmed().toStdString());
        database::discord_queries::WriteServerPrefix(dbToken->db, command.server->GetServerId(), QString::fromStdString(command.server->GetCommandPrefix()).trimmed());
        action->text = "Prefix has been changed";
    }
    else
        action->text = "Prefix wasn't changed because of an error";
    return action;
}
QSharedPointer<SendMessageCommand> InsufficientPermissionsAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command)
{
    action->text = "You don't have required permissions on the server to run this command.";
    return action;
}
QSharedPointer<SendMessageCommand> NullAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    if(command.variantHash.contains("reason"))
        action->text = command.variantHash["reason"].toString();
    return action;
}

QSharedPointer<SendMessageCommand> SetForcedListParamsAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    An<interfaces::Users> usersDbInterface;
    usersDbInterface->WriteForcedListParams(command.user->UserID(), command.variantHash["min"].toUInt(), command.variantHash["ratio"].toUInt());
    command.user->SetForcedMinMatch(command.variantHash["min"].toUInt());
    command.user->SetForcedRatio(command.variantHash["ratio"].toUInt());
    return action;
}

QSharedPointer<SendMessageCommand> SetForceLikedAuthorsAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    An<interfaces::Users> usersDbInterface;
    if(command.user->GetUseLikedAuthorsOnly())
        usersDbInterface->WriteForceLikedAuthors(command.user->UserID(), false);
    else
        usersDbInterface->WriteForceLikedAuthors(command.user->UserID(), true);
    command.user->SetUseLikedAuthorsOnly(!command.user->GetUseLikedAuthorsOnly());

    return action;
}


QSharedPointer<SendMessageCommand> ShowFreshRecommendationsAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    An<interfaces::Users> usersDbInterface;
    bool strict = command.variantHash.contains("strict");
    if(!command.user->GetSortFreshFirst() ||
            (strict && command.user->GetSortFreshFirst() && !command.user->GetStrictFreshSort())){
        usersDbInterface->WriteFreshSortingParams(command.user->UserID(), true, strict);
        action->text = "Fresh sorting mode turned on, to disable use the same command again.";
        if(command.user->GetSortFreshFirst())
            action->text = "Enabling strict mode for fresh sort.";
        command.user->SetSortFreshFirst(true);
        command.user->SetStrictFreshSort(strict);
    }
    else{
        usersDbInterface->WriteFreshSortingParams(command.user->UserID(), false, false);
        command.user->SetSortFreshFirst(false);
        command.user->SetStrictFreshSort(false);
        action->text = "Disabling fresh sort.";
    }
    return action;
}



QSharedPointer<SendMessageCommand> ShowCompleteAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    An<interfaces::Users> usersDbInterface;
    auto user = command.user;
    if(!user->GetShowCompleteOnly()){
        usersDbInterface->SetCompleteFilter(command.user->UserID(), true);
        user->SetShowCompleteOnly(true);
    }
    else{
        usersDbInterface->SetCompleteFilter(command.user->UserID(), false);
        user->SetShowCompleteOnly(false);
    }
    user->SetRngBustScheduled(true);
    return action;
}


QSharedPointer<SendMessageCommand> HideDeadAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    auto user = command.user;
    An<interfaces::Users> usersDbInterface;
    if(command.variantHash.contains("days")){
        int days = command.variantHash["days"].toUInt();
        user->SetDeadFicDaysRange(days);
        usersDbInterface->SetDeadFicDaysRange(command.user->UserID(), days);
        usersDbInterface->SetHideDeadFilter(command.user->UserID(), true);
        user->SetHideDead(true);
    }
    else{
        if(!user->GetHideDead()){
            usersDbInterface->SetHideDeadFilter(command.user->UserID(), true);
            user->SetHideDead(true);
        }
        else{
            usersDbInterface->SetHideDeadFilter(command.user->UserID(), false);
            user->SetHideDead(false);
        }
    }
    user->SetRngBustScheduled(true);
    return action;
}


QSharedPointer<SendMessageCommand> PurgeAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    An<interfaces::Users> usersDbInterface;
    auto user = command.user;
    An<Users> users;
    users->RemoveUserData(user);
    usersDbInterface->CompletelyRemoveUser(user->UserID());
    action->text = "Acknowledged: removing all your data from the database";
    return action;
}

QSharedPointer<SendMessageCommand> ResetFiltersAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    auto dbToken = An<discord::DatabaseVendor>()->GetDatabase("users");
    environment->fandoms->db = dbToken->db;
    An<interfaces::Users> usersDbInterface;
    auto user = command.user;
    user->SetHideDead(false);
    user->SetShowCompleteOnly(false);
    user->SetSortFreshFirst(false);
    user->SetStrictFreshSort(false);
    user->SetUseLikedAuthorsOnly(false);
    user->SetWordcountFilter({0,0});
    {
        usersDbInterface->SetHideDeadFilter(command.user->UserID(), false);
        usersDbInterface->SetCompleteFilter(command.user->UserID(), false);
        usersDbInterface->WriteFreshSortingParams(command.user->UserID(), false, false);
        usersDbInterface->WriteForceLikedAuthors(command.user->UserID(), false);
        usersDbInterface->SetWordcountFilter(command.user->UserID(), {0,0});
        auto currentFilter = command.user->GetCurrentFandomFilter();
        for(auto fandomId : std::as_const(currentFilter.fandoms))
            usersDbInterface->UnfilterFandom(command.user->UserID(), fandomId);
    }
    user->SetFandomFilter({});
    user->SetRngBustScheduled(true);
    action->text = "Done, filter has been reset.";
    return action;
}


QSharedPointer<SendMessageCommand> CreateSimilarFicListAction::ExecuteImpl(QSharedPointer<TaskEnvironment> environment, Command command)
{
    command.user->initNewEasyQuery();
    command.user->SetRngBustScheduled(true);
    auto ficId = command.ids.at(0);
    QSharedPointer<core::RecommendationList> listParams;
    //QString error;

    auto recList = CreateSimilarFicParams();
    recList->ignoreBreakdowns= true;

    QVector<core::Identity> pack;
    pack.resize(1);
    pack[0].web.ffn = ficId;
    environment->ficSource->ClearUserData();
    environment->ficSource->GetInternalIDsForFics(&pack);

    bool hasValidId = false;
    for(const auto& source: std::as_const(pack))
    {
        if(source.id > 0 )
            hasValidId = true;
        recList->ficData->sourceFics+=source.id;
        recList->ficData->fics+=source.web.ffn;
    }
    if(!hasValidId)
    {
        action->text = "Couldn't create the list. Server doesn't have any data for fic ID you supplied, either it's too new or too unpopular.";
        action->stopChain = true;
        return action;
    }

    environment->ficSource->GetRecommendationListFromServer(recList);

    // instantiating working set for user
    An<Users> users;
    command.user->SetFicList(recList->ficData);
    //QMap<int, int> scoreStatus; // maps maptch count to fic count with this match
    //QMap<int, QSet<int>> matchFicToScore; // maps maptch count to fic count with this match

    if(!recList->ficData->matchCounts.size())
    {
        action->text = "Couldn't create the list. Recommendations server is not available?";
        action->stopChain = true;
        return action;
    }


    action->text = "Created similarity list FFN ID: " + QString::number(command.ids.at(0));
    environment->ficSource->ClearUserData();
    return action;
}

QSharedPointer<SendMessageCommand> SetWordcountLimitAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command command)
{
    //auto ficIds = command.ids;
    command.user->SetWordcountFilter({command.ids.at(0)*1000,command.ids.at(1)*1000});
    An<interfaces::Users> usersDbInterface;
    usersDbInterface->SetWordcountFilter(command.user->UserID(),command.user->GetWordcountFilter());
    return action;
}



QSharedPointer<ActionBase> GetAction(ECommandType type)
{
    switch(type){
    case ECommandType::ct_ignore_fics:
        return QSharedPointer<ActionBase>(new IgnoreFicAction());
    case ECommandType::ct_ignore_fandoms:
        return QSharedPointer<ActionBase>(new IgnoreFandomAction());
    case ECommandType::ct_set_fandoms:
        return QSharedPointer<ActionBase>(new SetFandomAction());
    case ECommandType::ct_display_help:
        return QSharedPointer<ActionBase>(new GeneralHelpAction());
    case ECommandType::ct_display_page:
        return QSharedPointer<ActionBase>(new DisplayPageAction());
    case ECommandType::ct_timeout_active:
        return QSharedPointer<ActionBase>(new TimeoutActiveAction());
    case ECommandType::ct_fill_recommendations:
        return QSharedPointer<ActionBase>(new DesktopRecsCreationAction());
    case ECommandType::ct_no_user_ffn:
        return QSharedPointer<ActionBase>(new NoUserInformationAction());
    case ECommandType::ct_display_rng:
        return QSharedPointer<ActionBase>(new DisplayRngAction());
    case ECommandType::ct_change_server_prefix:
        return QSharedPointer<ActionBase>(new ChangePrefixAction());
    case ECommandType::ct_insufficient_permissions:
        return QSharedPointer<ActionBase>(new InsufficientPermissionsAction());
    case ECommandType::ct_force_list_params:
        return QSharedPointer<ActionBase>(new SetForcedListParamsAction());
    case ECommandType::ct_filter_liked_authors:
        return QSharedPointer<ActionBase>(new SetForceLikedAuthorsAction());
    case ECommandType::ct_show_favs:
        return QSharedPointer<ActionBase>(new ShowFullFavouritesAction());
    case ECommandType::ct_filter_fresh:
        return QSharedPointer<ActionBase>(new ShowFreshRecommendationsAction());
    case ECommandType::ct_filter_complete:
        return QSharedPointer<ActionBase>(new ShowCompleteAction());
    case ECommandType::ct_filter_out_dead:
        return QSharedPointer<ActionBase>(new HideDeadAction());
    case ECommandType::ct_purge:
        return QSharedPointer<ActionBase>(new PurgeAction());
    case ECommandType::ct_reset_filters:
        return QSharedPointer<ActionBase>(new ResetFiltersAction());
    case ECommandType::ct_create_similar_fics_list:
        return QSharedPointer<ActionBase>(new CreateSimilarFicListAction());
    case ECommandType::ct_create_recs_from_mobile_page:
        return QSharedPointer<ActionBase>(new MobileRecsCreationAction());
    case ECommandType::ct_set_wordcount_limit:
        return QSharedPointer<ActionBase>(new SetWordcountLimitAction());
    default:
        return QSharedPointer<ActionBase>(new NullAction());
    }
}

QSharedPointer<SendMessageCommand> ShowFullFavouritesAction::ExecuteImpl(QSharedPointer<TaskEnvironment>, Command)
{
    return {};
}









}








