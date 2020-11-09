#include "discord/command_generators.h"
#include "discord/client_v2.h"
#include "discord/discord_user.h"
#include "discord/help_generator.h"
#include "discord/discord_server.h"
#include "Interfaces/discord/users.h"
#include "include/grpc/grpc_source.h"
#include "logger/QsLog.h"
#include "discord/type_functions.h"
#include "discord/discord_message_token.h"
#include "GlobalHeaders/snippets_templates.h"
#include <stdexcept>

#include <QSettings>
namespace discord{
QStringList SendMessageCommand::tips = {};
QSharedPointer<User> CommandCreator::user;
void CommandChain::Push(Command&& command)
{
    commands.emplace_back(std::move(command));
}

void CommandChain::PushFront(Command&& command)
{
    commands.push_front(command);
}

void CommandChain::AddUserToCommands(QSharedPointer<User> user)
{
    for(auto& command : commands)
        command.user = user;
}

Command CommandChain::Pop()
{
    auto command = commands.back();
    commands.pop_back();
    return command;
}

CommandChain CommandCreator::ProcessInput(Client* client, QSharedPointer<discord::Server> server, const SleepyDiscord::Message& message , bool )
{
    result.Reset();
    this->client = client;
    this->server = server;
    return ProcessInputImpl(message);
}

void CommandCreator::AddFilterCommand(Command&& command)
{
    if(currentOperationRestoresActiveSet)
    {
        result.PushFront(std::move(command));
        currentOperationRestoresActiveSet = false;
    }
    else
        result.Push(std::move(command));
}

void CommandCreator::EnsureUserExists(QString userId, QString userName)
{
    An<Users> users;
    if(!users->HasUser(userId)){
        bool inDatabase = users->LoadUser(userId);
        if(!inDatabase)
        {
            QSharedPointer<discord::User> user(new discord::User(userId, QStringLiteral("-1"), userName, QUuid::createUuid().toString()));
            An<interfaces::Users> usersInterface;
            usersInterface->WriteUser(user);
            users->LoadUser(userId);
        }
    }
}


Command NewCommand(QSharedPointer<discord::Server> server, const SleepyDiscord::Message &message, ECommandType type){
    Command command;
    command.originalMessageToken = message;
    command.server = server;
    command.type = type;
    return command;
}

Command NewCommand(QSharedPointer<discord::Server> server, const MessageToken& message, ECommandType type){
    Command command;
    command.originalMessageToken = message;
    command.server = server;
    command.type = type;
    return command;
}


CommandChain RecommendationsCommand::ProcessInput(Client* client, QSharedPointer<discord::Server> server, const SleepyDiscord::Message& message, bool)
{
    result.Reset();
    this->client = client;
    this->server = server;

    An<Users> users;
    auto user = users->GetUser(QString::fromStdString(message.author.ID));
    if(!user->HasActiveSet()){
        if(user->FfnID().isEmpty() || user->FfnID() == QStringLiteral("-1"))
        {
            Command createRecs = NewCommand(server, message,ct_no_user_ffn);
            result.Push(std::move(createRecs));
            result.stopExecution = true;
            return std::move(result);
        }
        currentOperationRestoresActiveSet = true;

        Command createRecs = NewCommand(server, message,ct_fill_recommendations);
        createRecs.ids.push_back(user->FfnID().toInt());
        createRecs.variantHash[QStringLiteral("refresh")] = QStringLiteral("yes");
        createRecs.textForPreExecution = QString(QStringLiteral("Restoring recommendations for user %1 into an active set, please wait a bit")).arg(user->FfnID());
        result.hasParseCommand = true;
        result.Push(std::move(createRecs));
    }
    return ProcessInputImpl(message);
}

bool RecommendationsCommand::IsThisCommand(const std::string &)
{
    return false; //cmd == TypeStringHolder<RecommendationsCommand>::name;
}

std::once_flag settingsFlag;
CommandChain RecsCreationCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{

    static std::string ownerId;
    std::call_once(settingsFlag, [&](){
        QSettings settings(QStringLiteral("settings/settings_discord.ini"), QSettings::IniFormat);
        ownerId = settings.value(QStringLiteral("Login/ownerId")).toString().toStdString();
    });

    static constexpr int minRecsInterval = 60;
    if(user->secsSinceLastsRecQuery() < minRecsInterval && message.author.ID.string() != ownerId)
    {
        Command nullCommand = NewCommand(server, message,ct_timeout_active);
        nullCommand.ids.push_back(minRecsInterval-user->secsSinceLastsEasyQuery());
        nullCommand.variantHash[QStringLiteral("reason")] = QStringLiteral("Recommendations can only be regenerated once on 60 seconds.Please wait %1 more seconds.");
        nullCommand.originalMessageToken = message;
        nullCommand.server = this->server;
        result.Push(std::move(nullCommand));
        return std::move(result);
    }

    auto match = matchCommand<RecsCreationCommand>(message.content);
    auto refresh = match.get<1>().to_string();
    auto id = match.get<2>().to_string();

    Command createRecs = NewCommand(server, message,ct_fill_recommendations);
    if(id.length() == 0){
        if(!user->FfnID().isEmpty()){
            Command createRecs = NewCommand(server, message,ct_fill_recommendations);
            createRecs.ids.push_back(user->FfnID().toInt());
            createRecs.variantHash[QStringLiteral("refresh")] = true;
            createRecs.textForPreExecution = QString(QStringLiteral("Creating recommendations for ffn user %1. Please wait, depending on your list size, it might take a while.")).arg(user->FfnID());
            result.Push(std::move(createRecs));
            Command command = NewCommand(server, message,ct_display_page);
            command.ids.push_back(user->CurrentRecommendationsPage());
            result.Push(std::move(command));
            return std::move(result);
        }
        else
        {
            Command nullCommand = NewCommand(server, message,ct_null_command);
            nullCommand.variantHash[QStringLiteral("reason")] = QStringLiteral("Not a valid ID or user url.");
            nullCommand.originalMessageToken = message;
            nullCommand.server = this->server;
            result.Push(std::move(nullCommand));
            return std::move(result);
        }
    }
    user->SetSimilarFicsId(0);

    bool isNumConvertible = true;
    try
    {
        std::stoi(id);
    }
    catch (const std::invalid_argument& e)
    {
        isNumConvertible = false;
    }
    catch (const std::out_of_range& e)
    {
        isNumConvertible = false;
    }
    if(isNumConvertible)
        createRecs.ids.push_back(std::stoi(id));
    else
        createRecs.variantHash[QStringLiteral("url")] = QString::fromStdString(id);

    if(refresh.length() == 0)
    {
        createRecs.variantHash[QStringLiteral("refresh")] = true;
    }
    else{
        Command command = NewCommand(server, message,ct_force_list_params);
        command.variantHash[QStringLiteral("min")] = 0;
        command.variantHash[QStringLiteral("ratio")] = 0;
        result.Push(std::move(command));
    }
    createRecs.textForPreExecution = QString(QStringLiteral("Creating recommendations for ffn user %1. Please wait, depending on your list size, it might take a while.")).arg(QString::fromStdString(match.get<2>().to_string()));
    Command displayRecs = NewCommand(server, message,ct_display_page);
    displayRecs.ids.push_back(0);
    result.Push(std::move(createRecs));
    result.Push(std::move(displayRecs));
    result.hasParseCommand = true;
    return std::move(result);
}

bool RecsCreationCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<RecsCreationCommand>::name;
}

CommandChain PageChangeCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_display_page);
    auto match = matchCommand<PageChangeCommand>(message.content);
    if(match.get<1>().to_string().length() > 0)
    {
        command.ids.push_back(std::stoi(match.get<1>().to_string()));
        result.Push(std::move(command));
    }
    else
    {
        command.ids.push_back(user->CurrentRecommendationsPage());
        result.Push(std::move(command));
    }
    return std::move(result);
}

bool PageChangeCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<PageChangeCommand>::name;
}

CommandChain NextPageCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    An<Users> users;
    auto user = users->GetUser(QString::fromStdString(message.author.ID));

    Command displayRecs = NewCommand(server, message,ct_display_page);
    displayRecs.ids.push_back(user->CurrentRecommendationsPage()+1);
    result.Push(std::move(displayRecs));
    user->AdvancePage(1);
    return std::move(result);
}

bool NextPageCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<NextPageCommand>::name;
}

CommandChain PreviousPageCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    An<Users> users;
    auto user = users->GetUser(QString::fromStdString(message.author.ID));

    Command displayRecs = NewCommand(server, message,ct_display_page);
    auto newPage = user->CurrentRecommendationsPage()-1 < 0 ? 0 : user->CurrentRecommendationsPage()-1;
    displayRecs.ids.push_back(newPage);
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool PreviousPageCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<PreviousPageCommand>::name;
}

CommandChain SetFandomCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command filteredFandoms = NewCommand(server, message,ct_set_fandoms);
    auto match = matchCommand<SetFandomCommand>(message.content);
    auto pure = match.get<1>().to_string();
    auto reset = match.get<2>().to_string();
    auto fandom = match.get<3>().to_string();

    if(pure.length() > 0)
        filteredFandoms.variantHash[QStringLiteral("allow_crossovers")] = false;
    else
        filteredFandoms.variantHash[QStringLiteral("allow_crossovers")] = true;
    if(reset.length() > 0)
        filteredFandoms.variantHash[QStringLiteral("reset")] = true;
    filteredFandoms.variantHash[QStringLiteral("fandom")] = QString::fromStdString(fandom).trimmed();

    AddFilterCommand(std::move(filteredFandoms));
    Command displayRecs = NewCommand(server, message,user->GetLastPageType());
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool SetFandomCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<SetFandomCommand>::name;
}


CommandChain IgnoreFandomCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command ignoredFandoms = NewCommand(server, message,ct_ignore_fandoms);

    auto match = matchCommand<IgnoreFandomCommand>(message.content);
    auto full = match.get<1>().to_string();
    auto reset = match.get<2>().to_string();
    auto fandom = match.get<3>().to_string();

    if(full.length() > 0)
        ignoredFandoms.variantHash[QStringLiteral("with_crossovers")] = true;
    else
        ignoredFandoms.variantHash[QStringLiteral("with_crossovers")] = false;
    if(reset.length() > 0)
        ignoredFandoms.variantHash[QStringLiteral("reset")] = true;
    ignoredFandoms.variantHash[QStringLiteral("fandom")] = QString::fromStdString(fandom).trimmed();
    AddFilterCommand(std::move(ignoredFandoms));
    Command displayRecs = NewCommand(server, message,user->GetLastPageType());
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool IgnoreFandomCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<IgnoreFandomCommand>::name;
}

CommandChain IgnoreFicCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command ignoredFics = NewCommand(server, message,ct_ignore_fics);
    auto match = ctre::search<TypeStringHolder<IgnoreFicCommand>::patternCommand>(message.content);
    auto full = match.get<1>().to_string();
    bool silent = false;
    if(full.length() > 0){
        ignoredFics.variantHash[QStringLiteral("everything")] = true;
        ignoredFics.ids.clear();
    }
    else{
        for(auto match : ctre::range<TypeStringHolder<IgnoreFicCommand>::patternNum>(message.content)){
            auto silentStr = match.get<1>().to_string();
            if(silentStr.length() != 0)
                silent = true;
            auto numbers = QString::fromStdString(match.get<2>().to_string()).split(QStringLiteral(" "));
            for(const auto& number : numbers){
                auto id = number.toInt();
                if(id != 0){
                    ignoredFics.ids.push_back(id);
                }
            }
        }
    }

    AddFilterCommand(std::move(ignoredFics));
    if(!silent && (ignoredFics.variantHash.size() > 0 || ignoredFics.ids.size() > 0))
    {
        Command displayRecs = NewCommand(server, message,user->GetLastPageType());
        displayRecs.ids.push_back(0);
        displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
        result.Push(std::move(displayRecs));

    }
    return std::move(result);
}

bool IgnoreFicCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<IgnoreFicCommand>::name;
}

CommandChain SetIdentityCommand::ProcessInputImpl(const SleepyDiscord::Message&)
{
//    Command command;
//    command.type = ct_set_identity;
//    auto match = matches.next();
//    command.ids.push_back(match.captured(1).toUInt());
//    command.originalMessage = message;
//    result.Push(command);
    return std::move(result);
}

bool SetIdentityCommand::IsThisCommand(const std::string &)
{
    return false;//cmd == TypeStringHolder<SetIdentityCommand>::name;
}

CommandChain CommandParser::Execute(const std::string& command, QSharedPointer<discord::Server> server, const SleepyDiscord::Message& message)
{
    std::lock_guard<std::mutex> guard(lock);
    bool firstCommand = true;
    CommandChain result;

    CommandCreator::EnsureUserExists(QString::fromStdString(message.author.ID), QString::fromStdString(message.author.username));
    An<Users> users;
    auto user = users->GetUser(QString::fromStdString(message.author.ID.string()));
    if(user->secsSinceLastsEasyQuery() < 3)
    {
        Command command = NewCommand(server, message,ct_timeout_active);
        command.ids.push_back(3-user->secsSinceLastsEasyQuery());
        command.variantHash[QStringLiteral("reason")] = QStringLiteral("One command can be issued each 3 seconds. Please wait %1 more seconds.");
        command.user = user;
        result.Push(std::move(command));
        result.stopExecution = true;
        return result;
    }

    for(auto& processor: commandProcessors)
    {
        if(!processor->IsThisCommand(command))
            continue;

        processor->user = user;
        auto newCommands = processor->ProcessInput(client, server, message, firstCommand);
        result += newCommands;
        if(newCommands.hasParseCommand)
            result.hasParseCommand = true;
        if(newCommands.stopExecution == true)
            break;
        if(firstCommand)
            firstCommand = false;
        break;
    }
    result.AddUserToCommands(user);
    for(const auto& command: std::as_const(result.commands)){
        if(!command.textForPreExecution.isEmpty())
            client->sendMessage(command.originalMessageToken.channelID, command.textForPreExecution.toStdString());
    }
    return result;
}

CommandChain DisplayHelpCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_display_help);

    auto match = ctre::search<TypeStringHolder<DisplayHelpCommand>::pattern>(message.content);
    auto helpTarget = match.get<2>().to_string();
    auto view = std::string_view(helpTarget);
    auto prefix = server->GetCommandPrefix();
    if(view.substr(0, prefix.length()) == prefix)
        view.remove_prefix(server->GetCommandPrefix().length());
    if(view.length() > 0)
        command.ids.push_back(GetHelpPage(view));
    else
        command.ids.push_back(0);
    result.Push(std::move(command));
    return std::move(result);
}

bool DisplayHelpCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<DisplayHelpCommand>::name;
}

void SendMessageCommand::Invoke(Client * client)
{
    try{
        auto addReaction = [&](const SleepyDiscord::Message& newMessage){
            for(const auto& reaction: std::as_const(reactionsToAdd))
                client->addReaction(originalMessageToken.channelID, newMessage.ID, reaction.toStdString());
        };
        if(targetMessage.string().length() == 0){
            if(embed.empty())
            {
                SleepyDiscord::Embed embed;
                if(text.length() > 0)
                {
                    auto resultingMessage = client->sendMessage(originalMessageToken.channelID, text.toStdString(), embed).cast();
                    // I don't need to add reactions or hash messages without filled embeds
                }
            }
            else{
                auto resultingMessage = client->sendMessage(originalMessageToken.channelID, text.toStdString(), embed).cast();

                // I only need to hash messages that the user can later react to
                // meaning page, rng and help commands
                if(originalCommandType *in(ct_display_page, ct_display_rng, ct_display_help)){
                    if(originalCommandType *in(ct_display_page, ct_display_rng))
                        this->user->SetLastPageMessage({resultingMessage, originalMessageToken.channelID});
                    client->messageSourceAndTypeHash.push(resultingMessage.ID.number(),{originalMessageToken, originalCommandType});
                    addReaction(resultingMessage);
                }
            }
        }
        else{
            // editing message doesn't change its Id so rehashing isn't required
            client->editMessage(originalMessageToken.channelID, targetMessage, text.toStdString(), embed);
        }
        if(!diagnosticText.isEmpty())
            client->sendMessage(originalMessageToken.channelID, diagnosticText.toStdString());
    }
    catch (const SleepyDiscord::ErrorCode& error){
        QLOG_INFO() << "Discord error:" << error;
        QLOG_INFO() << "Discord error:" << QString::fromStdString(this->embed.description);
    }

}


CommandChain RngCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_display_rng);
    auto match = ctre::search<TypeStringHolder<RngCommand>::pattern>(message.content);
    command.variantHash[QStringLiteral("quality")] = QString::fromStdString(match.get<1>().to_string()).trimmed();
    result.Push(std::move(command));
    user->SetSimilarFicsId(0);
    return std::move(result);
}

bool RngCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<RngCommand>::name;
}

CommandChain ChangeServerPrefixCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    SleepyDiscord::Server sleepyServer = client->getServer(this->server->GetServerId());
    //std::list<SleepyDiscord::ServerMember>::iterator member = sleepyServer.findMember(message.author.ID);
    const auto& member = client->getMember(this->server->GetServerId(), message.author.ID).cast();
    bool isAdmin = false;
    auto roles = member.roles;
    for(auto& roleId : roles){
        auto role = sleepyServer.findRole(roleId);
        auto permissions = role->permissions;
        if(SleepyDiscord::hasPremission(permissions, SleepyDiscord::ADMINISTRATOR))
        {
            isAdmin = true;
            break;
        }
    }
    if(isAdmin || sleepyServer.ownerID == message.author.ID)
    {
        Command command = NewCommand(server, message,ct_change_server_prefix);
        auto match = ctre::search<TypeStringHolder<ChangeServerPrefixCommand>::pattern>(message.content);
        auto newPrefix = QString::fromStdString(match.get<1>().to_string()).trimmed();
        if(newPrefix.isEmpty())
        {
            command.type = ct_null_command;
            command.textForPreExecution = QStringLiteral("prefix cannot be empty");
            result.Push(std::move(command));
            return std::move(result);
        }
        command.variantHash[QStringLiteral("prefix")] = newPrefix;
        command.textForPreExecution = QStringLiteral("Changing prefix for this server to: ") + newPrefix;
        result.Push(std::move(command));
    }
    else
    {
        Command command = NewCommand(server, message,ct_insufficient_permissions);
        result.Push(std::move(command));
    }
    return std::move(result);
}

bool ChangeServerPrefixCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<ChangeServerPrefixCommand>::name;
}

//ForceListParamsCommand::ForceListParamsCommand()
//{

//}

//CommandChain ForceListParamsCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
//{
//    Command command = NewCommand(server, message,ct_force_list_params);
//    auto match = ctre::search<TypeStringHolder<ForceListParamsCommand>::pattern>(message.content);
//    auto min = match.get<1>().to_string();
//    auto ratio = match.get<2>().to_string();
//    if(min.length() == 0 || ratio.length() == 0)
//        return std::move(result);

//    An<Users> users;
//    auto user = users->GetUser(QString::fromStdString(message.author.ID));
//    if(user->FfnID().isEmpty() || user->FfnID() == "-1")
//    {
//        Command createRecs = NewCommand(server, message,ct_no_user_ffn);
//        result.Push(createRecs);
//        result.stopExecution = true;
//        return std::move(result);
//    }

//    command.variantHash["min"] = std::stoi(min);
//    command.variantHash["ratio"] = std::stoi(ratio);
//    AddFilterCommand(command);
//    Command displayRecs = NewCommand(server, message,ct_display_page);
//    displayRecs.variantHash["refresh_previous"] = true;
//    displayRecs.ids.push_back(0);
//    result.Push(displayRecs);
//    return std::move(result);
//}

//bool ForceListParamsCommand::IsThisCommand(const std::string &cmd)
//{
//    return cmd == TypeStringHolder<ForceListParamsCommand>::name;
//}


CommandChain FilterLikedAuthorsCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_filter_liked_authors);
    auto match = ctre::search<TypeStringHolder<FilterLikedAuthorsCommand>::pattern>(message.content);
    if(match)
    {
        command.variantHash[QStringLiteral("liked")] = true;
        AddFilterCommand(std::move(command));
        Command displayRecs = NewCommand(server, message,user->GetLastPageType());
        displayRecs.ids.push_back(0);
        displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
        result.Push(std::move(displayRecs));
    }
    return std::move(result);
}

bool FilterLikedAuthorsCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<FilterLikedAuthorsCommand>::name;
}

//CommandChain ShowFullFavouritesCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
//{
//    Command command = NewCommand(server, message,ct_show_favs);
//    auto match = ctre::search<TypeStringHolder<ShowFullFavouritesCommand>::pattern>(message.content);
//    if(match)
//        result.Push(command);
//    return std::move(result);
//}

//bool ShowFullFavouritesCommand::IsThisCommand(const std::string &cmd)
//{
//    return cmd == TypeStringHolder<ShowFullFavouritesCommand>::name;
//}

CommandChain ShowFreshRecsCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_filter_fresh);
    auto match = ctre::search<TypeStringHolder<ShowFreshRecsCommand>::pattern>(message.content);
    auto strict = match.get<1>().to_string();
    if(strict.length() > 0)
        command.variantHash[QStringLiteral("strict")] = true;
    AddFilterCommand(std::move(command));

    Command displayRecs = NewCommand(server, message,ct_display_page);
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool ShowFreshRecsCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<ShowFreshRecsCommand>::name;
}

CommandChain ShowCompletedCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_filter_complete);
    AddFilterCommand(std::move(command));

    Command displayRecs = NewCommand(server, message,user->GetLastPageType());
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool ShowCompletedCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<ShowCompletedCommand>::name;
}

CommandChain HideDeadCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_filter_out_dead);
    auto match = ctre::search<TypeStringHolder<HideDeadCommand>::pattern>(message.content);
    auto strict = match.get<1>().to_string();
    if(strict.length() > 0)
    {
        auto number = std::stoi(match.get<1>().to_string());
        if(number > 0)
        {
            static const int reasonableDeadLimits = 36500;
            // 100 years should be enough, lmao
            if(number > reasonableDeadLimits)
                number = reasonableDeadLimits;
            command.variantHash[QStringLiteral("days")] = number;
        }
        else
        {
            Command nullCommand = NewCommand(server, message,ct_null_command);
            nullCommand.type = ct_null_command;
            nullCommand.variantHash[QStringLiteral("reason")] = QStringLiteral("Number of days must be greater than 0");
            nullCommand.originalMessageToken = message;
            nullCommand.server = this->server;
            result.Push(std::move(nullCommand));
            return std::move(result);
        }
    }

    AddFilterCommand(std::move(command));

    Command displayRecs = NewCommand(server, message,user->GetLastPageType());
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool HideDeadCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<HideDeadCommand>::name;
}

CommandChain PurgeCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_purge);
    result.Push(std::move(command));
    return std::move(result);
}

bool PurgeCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<PurgeCommand>::name;
}


CommandChain ResetFiltersCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_reset_filters);
    AddFilterCommand(std::move(command));
    Command displayRecs = NewCommand(server, message,user->GetLastPageType());
    displayRecs.ids.push_back(0);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool ResetFiltersCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<ResetFiltersCommand>::name;
}



CommandChain CreateRollCommand(QSharedPointer<User> user, QSharedPointer<Server> server, const MessageToken& message){
    CommandChain result;
    Command command = NewCommand(server, message,ct_display_rng);
    command.variantHash[QStringLiteral("quality")] = user->GetLastUsedRoll();
    command.targetMessage = message.messageID;
    command.user = user;
    result.Push(std::move(command));
    return result;
}

CommandChain CreateChangeRecommendationsPageCommand(QSharedPointer<User> user, QSharedPointer<Server> server, const MessageToken& message, bool shiftRight)
{
    CommandChain result;
    Command command = NewCommand(server, message,ct_display_page);
    if(shiftRight)
        command.ids.push_back(user->CurrentRecommendationsPage() + 1);
    else if(user->CurrentRecommendationsPage() != 0)
        command.ids.push_back(user->CurrentRecommendationsPage() - 1);
    else
        return result;

    command.targetMessage = message.messageID;
    command.user = user;
    result.Push(std::move(command));
    return result;
}

CommandChain CreateChangeHelpPageCommand(QSharedPointer<User> user, QSharedPointer<Server> server, const MessageToken& message, bool shiftRight)
{
    CommandChain result;
    Command command = NewCommand(server, message,ct_display_help);
    int maxHelpPages = static_cast<int>(discord::EHelpPages::last_help_page) + 1;
    int newPage = 0;
    if(shiftRight)
    {
        newPage = user->GetCurrentHelpPage() + 1;
        if(newPage == maxHelpPages)
            newPage = 0;
    }
    else {
        newPage = user->GetCurrentHelpPage() - 1;
        if(newPage < 0)
            newPage = maxHelpPages - 1;

    }
    command.ids.push_back(newPage);
    command.targetMessage = message.messageID;
    command.user = user;
    result.Push(std::move(command));
    return result;
}

CommandChain SimilarFicsCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    CommandChain result;
    Command command = NewCommand(server, message,ct_create_similar_fics_list);

    auto match = matchCommand<SimilarFicsCommand>(message.content);
    auto ficId = match.get<1>().to_string();
    if(ficId.length() == 0)
    {
        if(user->GetSimilarFicsId() != 0){
            user->SetSimilarFicsId(0);

            Command createRecs = NewCommand(server, message,ct_fill_recommendations);
            createRecs.ids.push_back(user->FfnID().toUInt());
            createRecs.variantHash[QStringLiteral("refresh")] = true;
            createRecs.textForPreExecution = QString(QStringLiteral("Creating recommendations for ffn user %1. Please wait, depending on your list size, it might take a while.")).arg(user->FfnID());
            result.Push(std::move(createRecs));
            Command displayRecs = NewCommand(server, message,ct_display_page);
            displayRecs.ids.push_back(0);
            result.Push(std::move(displayRecs));
            return result;
        }
    }
    user->SetSimilarFicsId(std::stoi(match.get<1>().to_string()));
    command.ids.push_back(std::stoi(match.get<1>().to_string()));
    result.Push(std::move(command));
    Command displayRecs = NewCommand(server, message,ct_display_page);
    displayRecs.ids.push_back(0);
    result.Push(std::move(displayRecs));
    return result;

}

bool SimilarFicsCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<SimilarFicsCommand>::name;
}


CommandChain WordcountCommand::ProcessInputImpl(const SleepyDiscord::Message& message)
{
    Command command = NewCommand(server, message,ct_set_wordcount_limit);
    auto match = ctre::search<TypeStringHolder<WordcountCommand>::pattern>(message.content);
    auto filterType = match.get<2>().to_string();
    auto rangeBegin = match.get<3>().to_string();
    auto rangeEnd = match.get<4>().to_string();

    if(filterType.length() == 0)
        filterType = "reset";

    auto pushNullCommand = [&](const QString& reason){
        Command nullCommand = NewCommand(server, message,ct_null_command);
        nullCommand.type = ct_null_command;
        nullCommand.variantHash[QStringLiteral("reason")] = reason;
        nullCommand.originalMessageToken = message;
        nullCommand.server = this->server;
        result.Push(std::move(nullCommand));
    };
    if((filterType == "less" || filterType == "more") && rangeBegin.length() == 0)
    {
        pushNullCommand(QStringLiteral("`less` and `more` commands require a desired wordcount after them."));
        return std::move(result);
    }
    if(filterType == "between" && (rangeBegin.length() == 0 || rangeEnd.length() == 0))
    {
        pushNullCommand(QStringLiteral("`between` command requires both being and end of the desied wordcount range."));
        return std::move(result);
    }

    if(filterType == "less"){
        command.ids.push_back(0);
        command.ids.push_back(std::stoi(rangeBegin));
    }
    else if(filterType == "more")
    {
        command.ids.push_back(std::stoi(rangeBegin));
        command.ids.push_back(std::numeric_limits<int>::max());
    }
    else if(filterType == "between"){
        command.ids.push_back(std::stoi(rangeBegin));
        command.ids.push_back(std::stoi(rangeEnd));
    }
    else{
        command.ids.push_back(0);
        command.ids.push_back(0);
    }
    AddFilterCommand(std::move(command));
    Command displayRecs = NewCommand(server, message,ct_display_page);
    displayRecs.variantHash[QStringLiteral("refresh_previous")] = true;
    displayRecs.ids.push_back(0);
    result.Push(std::move(displayRecs));
    return std::move(result);
}

bool WordcountCommand::IsThisCommand(const std::string &cmd)
{
    return cmd == TypeStringHolder<WordcountCommand>::name;
}





}


