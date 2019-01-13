/*
Flipper is a replacement search engine for fanfiction.net search results
Copyright (C) 2017-2018  Marchenko Nikolai

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "tasks/author_genre_iteration_processor.h"
#include "include/url_utils.h"
#include "include/page_utils.h"
#include "include/Interfaces/genres.h"
#include "include/timeutils.h"
#include "include/statistics_utils.h"

#include <QThread>
#include <QtConcurrent>

AuthorGenreIterationProcessor::AuthorGenreIterationProcessor()
{
}

struct AuthorGenreData{
    void Clear(){
        genreFactors = std::array<double, 22>{};
    }
    int authorId = -1;
    std::array<double, 22> genreFactors;
};

struct IteratorTask{
    QHash<int, Roaring>::iterator start;
    QHash<int, Roaring>::iterator end;
};

void AuthorGenreIterationProcessor::ReprocessGenreStats(QHash<int, QList<genre_stats::GenreBit> > inputFicData,
                                                        QHash<int, Roaring> inputAuthorData)
{
    statistics_utils::UserPageSource source;
    statistics_utils::UserPageSink<AuthorGenreData> sink;

    QList<IteratorTask> iteratorTasks;
    //int processingThreads = QThread::idealThreadCount()-1;
    int processingThreads = 1;
    int chunkSize = inputAuthorData.size()/processingThreads;
    for(int i = 0; i< processingThreads; i ++)
    {
        iteratorTasks.push_back({inputAuthorData.begin()+i*chunkSize,
                                 i == processingThreads ? inputAuthorData.end() : inputAuthorData.begin()+(i+1)*chunkSize});
    }

    sink.tokens.reserve(inputAuthorData.size());
    auto processor = [](IteratorTask task, const QHash<int, QList<genre_stats::GenreBit>> inputFicData) -> QList<AuthorGenreData> {
        auto it = task.start;
        QList<AuthorGenreData> result;
        result.reserve(std::distance(task.start, task.end));
        An<interfaces::GenreIndex> genreIndex;
        thread_local AuthorGenreData data;
        thread_local QHash<QString, float> genreKeeper;
        while(it != task.end)
        {
            data.Clear();
            genreKeeper.clear();
            //interfaces::Genres::LogGenreDistribution(data.genreFactors, "this is supposed to be clean");
            data.authorId = it.key();
            int ficTotal = it.value().cardinality();
            //QLOG_INFO() << "Total fics for author id: " << it.key() << " : " << ficTotal;
            for(auto ficId : it.value())
            {
                QHash<int, QList<genre_stats::GenreBit>>::const_iterator ficIt = inputFicData.find(ficId);
                if(ficIt == inputFicData.end())
                {
                    QLOG_INFO() << "No data for fic: " << ficId;
                    continue;
                }
                QString log = "genres for fic: " +  QString::number(ficId) + " ";
                for(auto genreBit: ficIt.value())
                {
                    for(auto actualBit: genreBit.genres)
                    {
                        //if(genreBit.isInTheOriginal)
                        log +=  "{"  + actualBit + " " + QString::number(genreBit.relevance) + "} ";
                        if(genreBit.isInTheOriginal)
                            genreKeeper[actualBit] += genreBit.relevance;
                    }
                }
                //QLOG_INFO() << log;
            }

            //QLOG_INFO() << "genre keeper: " << genreKeeper;

            for(auto genre : genreKeeper.keys())
            {
                double factor = static_cast<double>(genreKeeper[genre])/static_cast<double>(ficTotal);
                //QLOG_INFO() << "Appending value of: " << genre << " " << factor;
                data.genreFactors[genreIndex->IndexByFFNName(genre)] += factor;
                //interfaces::Genres::LogGenreDistribution(data.genreFactors);
            }
            //QLOG_INFO() << "Genre distribution for author: " << data.authorId;
            //interfaces::Genres::LogGenreDistribution(data.genreFactors);
            result.push_back(data);
            it++;
        }
        return result;
    };

    QList<QFuture<QList<AuthorGenreData>>> futures;
    for(int i = 0; i < processingThreads; i++)
    {
        futures.push_back(QtConcurrent::run(processor, iteratorTasks[i], inputFicData));
    }
    for(auto future: futures)
    {
        future.waitForFinished();
    }
    for(auto future: futures)
        for(auto data : future.result())
            resultingGenreAuthorData[data.authorId] = data.genreFactors;


    An<interfaces::GenreIndex> genreIndex;
    for(auto key: resultingGenreAuthorData.keys())
    {
        auto& genreData = resultingGenreAuthorData[key];
        genre_stats::ListMoodData moodData;
        moodData.strengthBondy = static_cast<float>(
                    genreData[genreIndex->IndexByFFNName("Friendship")] + genreData[genreIndex->IndexByFFNName("Family")]
                );
        moodData.strengthFunny = static_cast<float>(
                    genreData[genreIndex->IndexByFFNName("Humor")] + genreData[genreIndex->IndexByFFNName("Parody")]
                );
        moodData.strengthHurty = static_cast<float>(
                    genreData[genreIndex->IndexByFFNName("Hurt/Comfort")]
                );
        moodData.strengthFlirty= static_cast<float>(
                    genreData[genreIndex->IndexByFFNName("Romance")]
                );
        moodData.strengthNeutral= static_cast<float>(
                genreData[genreIndex->IndexByFFNName("Adventure")] + genreData[genreIndex->IndexByFFNName("Sci-Fi")] +
                genreData[genreIndex->IndexByFFNName("Spiritual")] + genreData[genreIndex->IndexByFFNName("Supernatural")] +
                genreData[genreIndex->IndexByFFNName("Suspense")] + genreData[genreIndex->IndexByFFNName("Mystery")] +
                genreData[genreIndex->IndexByFFNName("Crime")] + genreData[genreIndex->IndexByFFNName("Fantasy")] +
                genreData[genreIndex->IndexByFFNName("Western")]
                );
        moodData.strengthDramatic = static_cast<float>(
                    genreData[genreIndex->IndexByFFNName("Drama")] + genreData[genreIndex->IndexByFFNName("Tragedy")] + genreData[genreIndex->IndexByFFNName("Angst")]
                );
        moodData.listId = key;
        //moodData.Log();
        resultingMoodAuthorData[key] = moodData;

    }

}

