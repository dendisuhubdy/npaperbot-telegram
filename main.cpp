#include <CLI/CLI.hpp>

#include <tgbot/net/CurlHttpClient.h>
#include <tgbot/tgbot.h>

#include <spdlog/logger.h>
#include <spdlog/sinks/daily_file_sink.h>

#include <nlohmann/json.hpp>

#include <boost/algorithm/string.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

std::mutex papersDatabase;

void updatePapersDatabase(TgBot::HttpClient const& httpClient, nlohmann::json& papers,
    const std::string& papersDatabaseAddress)
{
    std::vector<TgBot::HttpReqArg> args;

    const TgBot::Url uri(papersDatabaseAddress);

    const std::string result = httpClient.makeRequest(uri, args);

    std::lock_guard<std::mutex> lockGuard(papersDatabase);
    papers = nlohmann::json::parse(result);
}

int main(int argc, char* argv[])
{
    CLI::App app("nPaperBot Telegram");

    std::string token;
    app.add_option("--token", token, "Telegram Bot API token")->required();

    int MaxResultCount = 20;
    app.add_option("--max-results-count", MaxResultCount, "Maximum results count per request");

    int MaxMessageLength = 2500;
    app.add_option("--max-message-length", MaxMessageLength, "Maximum result message length");

    std::string PapersDatabaseAddress = "https://raw.githubusercontent.com/wg21link/db/master/index.json";
    app.add_option("--database-address", PapersDatabaseAddress, "Online database address with papers");

    std::string LogsPath = "logs/log.txt";
    app.add_option("--log-path", LogsPath, "Path to log folder");

    std::optional<std::string> CertificatePath;
    app.add_option("--ca-info", CertificatePath, "Path to a certificate");

    CLI11_PARSE(app, argc, argv);

    TgBot::CurlHttpClient httpClient;
    // Curl configuration
    if(CertificatePath.has_value())
    {
        curl_easy_setopt(httpClient.curlSettings, CURLOPT_CAINFO, CertificatePath.value().c_str());
    }

    auto daily_logger = spdlog::daily_logger_mt("daily_logger", LogsPath, 0, 0);
    daily_logger->flush_on(spdlog::level::info);

    nlohmann::json papers;
    updatePapersDatabase(httpClient, papers, PapersDatabaseAddress);

    std::thread updatePapersThread([&httpClient, &papers, &PapersDatabaseAddress, daily_logger]()
        {
            while(true)
            {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(std::chrono::duration(60min));

                daily_logger->info("Update database started");
                updatePapersDatabase(httpClient, papers, PapersDatabaseAddress);
                daily_logger->info("Update database finished");
            }
        });
    updatePapersThread.detach();

    TgBot::Bot bot(token, httpClient);
    bot.getEvents().onCommand("paper", [&bot, &papers, MaxResultCount, MaxMessageLength, daily_logger](TgBot::Message::Ptr message)
        {
            daily_logger->info("Paper command requested with following text: " + message->text);

            std::string fixedMessage = message->text.substr();

            // Filter out from request command name
            fixedMessage.erase(fixedMessage.begin(), fixedMessage.begin() + fixedMessage.find(' ') + 1);

            const std::string ResultFiller = "For the request \"" +  fixedMessage + "\":\n";
            std::string result = ResultFiller;
            bool anyResult = false;
            int resultCount = 0;

            std::lock_guard<std::mutex> lockGuard(papersDatabase);
            for(auto const& paperObject : papers.items())
            {
                const auto paper = paperObject.value();
                // If we cannot find any supported field - just skip this paper
                if(paper.find("type") == paper.end() || paper.find("title") == paper.end() ||
                    paper.find("author") == paper.end() || paper.find("link") == paper.end() ||
                    paper["type"].get<std::string>() != "paper")
                {
                    continue;
                }

                // Search by paper name, title and author
                const auto paperName = paperObject.key();
                const auto paperTitle = paper["title"].get<std::string>();
                const auto paperAuthor = paper["author"].get<std::string>();

                // Do case-insensitive search
                if(boost::algorithm::icontains(paperName, fixedMessage) ||
                   boost::algorithm::icontains(paperTitle, fixedMessage) ||
                   boost::algorithm::icontains(paperAuthor, fixedMessage))
                {
                    if(resultCount == MaxResultCount)
                    {
                        daily_logger->info("Reached maximum result count");
                        result += "There are more papers. Please use more precise query.";
                        break;
                    }

                    anyResult = true;
                    ++resultCount;
                    result += paperName + ": " + paper["title"].get<std::string>() + " from " +
                              paper["author"].get<std::string>() + "\n" + paper["link"].get<std::string>() + "\n\n";

                    if(result.size() > MaxMessageLength)
                    {
                        daily_logger->info("Reached maximum message length");
                        bot.getApi().sendMessage(message->chat->id, result);
                        result = ResultFiller;
                    }
                }
            }

            // If we found nothing
            if(!anyResult)
            {
                daily_logger->info("No result");
                result +=  "Found nothing. Sorry.";
            }

            // If we found something - return the result
            if(result != ResultFiller)
            {
                daily_logger->info("Successful response");
                bot.getApi().sendMessage(message->chat->id, result);
            }
        });

    bot.getEvents().onCommand("help", [&bot, daily_logger](TgBot::Message::Ptr message)
    {
        daily_logger->info("Help command requested");

        bot.getApi().sendMessage(message->chat->id, "Use \"/paper\" command with substring from a proposal title."
                                                    "Search works only for paper name, titles and authors. "
                                                    "Search works as finding a substring in a string."
                                                    "Fuzzy search isn't supported yet.");
    });

    try
    {
        std::cout << "Bot username: " << bot.getApi().getMe()->username << std::endl;
        TgBot::TgLongPoll longPoll(bot, 100, 10);
        while (true)
        {
            std::cout << "Long poll started\n";
            longPoll.start();
        }
    }
    catch (const TgBot::TgException& e)
    {
        std::cout << "Telegram bot exception: " << e.what() << std::endl;
    }
    catch(const std::exception& e)
    {
        std::cout << "Exception: " << e.what() << std::endl;
    }
    return 0;
}