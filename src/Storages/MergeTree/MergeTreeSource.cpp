#include <Storages/MergeTree/MergeTreeSource.h>
#include <Storages/MergeTree/MergeTreeBaseSelectProcessor.h>
#include <Interpreters/threadPoolCallbackRunner.h>
#include <IO/IOThreadPool.h>
#include <Common/EventFD.h>

namespace DB
{

MergeTreeSource::MergeTreeSource(MergeTreeSelectAlgorithmPtr algorithm_)
    : ISource(algorithm_->getHeader())
    , algorithm(std::move(algorithm_))
{
#if defined(OS_LINUX)
    if (algorithm->getSettings().use_asynchronous_read_from_pool)
        async_reading_state = std::make_unique<AsyncReadingState>();
#endif
}

MergeTreeSource::~MergeTreeSource() = default;

std::string MergeTreeSource::getName() const
{
    return algorithm->getName();
}

void MergeTreeSource::onCancel()
{
    algorithm->cancel();
}

#if defined(OS_LINUX)
struct MergeTreeSource::AsyncReadingState
{
    /// NotStarted -> InProgress -> IsFinished -> NotStarted ...
    enum class Stage
    {
        NotStarted,
        InProgress,
        IsFinished,
    };

    struct Control
    {
        /// setResult and setException are the only methods
        /// which can be called from background thread.
        /// Invariant:
        ///   * background thread changes status InProgress -> IsFinished
        ///   * (status == InProgress) => (MergeTreeBaseSelectProcessor is alive)

        void setResult(ChunkAndProgress chunk_)
        {
            chassert(stage == Stage::InProgress);
            chunk = std::move(chunk_);
            finish();
        }

        void setException(std::exception_ptr exception_)
        {
            chassert(stage == Stage::InProgress);
            exception = exception_;
            finish();
        }

    private:

        /// Executor requires file descriptor (which can be polled) to be returned for async execution.
        /// We are using EventFD here.
        /// Thread from background pool writes to fd when task is finished.
        /// Working thread should read from fd when task is finished or canceled to wait for bg thread.
        EventFD event;
        std::atomic<Stage> stage = Stage::NotStarted;

        ChunkAndProgress chunk;
        std::exception_ptr exception;

        void finish()
        {
            stage = Stage::IsFinished;
            event.write();
        }

        ChunkAndProgress getResult()
        {
            chassert(stage == Stage::IsFinished);
            event.read();
            stage = Stage::NotStarted;

            if (exception)
                std::rethrow_exception(exception);

            return std::move(chunk);
        }

        friend struct AsyncReadingState;
    };

    std::shared_ptr<Control> start()
    {
        chassert(control->stage == Stage::NotStarted);
        control->stage = Stage::InProgress;
        return control;
    }

    void schedule(ThreadPool::Job job)
    {
        callback_runner(std::move(job), 0);
    }

    ChunkAndProgress getResult()
    {
        return control->getResult();
    }

    Stage getStage() const { return control->stage; }
    int getFD() const { return control->event.fd; }

    AsyncReadingState()
    {
        control = std::make_shared<Control>();
        callback_runner = threadPoolCallbackRunner<void>(IOThreadPool::get(), "MergeTreeRead");
    }

    ~AsyncReadingState()
    {
        /// Here we wait for async task if needed.
        /// ~AsyncReadingState and Control::finish can be run concurrently.
        /// It's important to store std::shared_ptr<Control> into bg pool task.
        /// Otherwise following is possible:
        ///
        ///  (executing thread)                         (bg pool thread)
        ///                                             Control::finish()
        ///                                             stage = Stage::IsFinished;
        ///  ~MergeTreeBaseSelectProcessor()
        ///  ~AsyncReadingState()
        ///  control->stage != Stage::InProgress
        ///  ~EventFD()
        ///                                             event.write()
        if (control->stage == Stage::InProgress)
            control->event.read();
    }

private:
    ThreadPoolCallbackRunner<void> callback_runner;
    std::shared_ptr<Control> control;
};
#endif

ISource::Status MergeTreeSource::prepare()
{
#if defined(OS_LINUX)
    if (!async_reading_state)
        return ISource::prepare();

    /// Check if query was cancelled before returning Async status. Otherwise it may lead to infinite loop.
    if (isCancelled())
    {
        getPort().finish();
        return ISource::Status::Finished;
    }

    if (async_reading_state && async_reading_state->getStage() == AsyncReadingState::Stage::InProgress)
        return ISource::Status::Async;
#endif

    return ISource::prepare();
}


std::optional<Chunk> MergeTreeSource::reportProgress(ChunkAndProgress chunk)
{
    if (chunk.num_read_rows || chunk.num_read_bytes)
        progress(chunk.num_read_rows, chunk.num_read_bytes);

    if (chunk.chunk.hasRows())
        return std::move(chunk.chunk);

    return {};
}


std::optional<Chunk> MergeTreeSource::tryGenerate()
{
#if defined(OS_LINUX)
    if (async_reading_state)
    {
        if (async_reading_state->getStage() == AsyncReadingState::Stage::IsFinished)
            return reportProgress(async_reading_state->getResult());

        chassert(async_reading_state->getStage() == AsyncReadingState::Stage::NotStarted);

        /// It is important to store control into job.
        /// Otherwise, race between job and ~MergeTreeBaseSelectProcessor is possible.
        auto job = [this, control = async_reading_state->start()]() mutable
        {
            auto holder = std::move(control);

            try
            {
                holder->setResult(algorithm->read());
            }
            catch (...)
            {
                holder->setException(std::current_exception());
            }
        };

        async_reading_state->schedule(std::move(job));

        return Chunk();
    }
#endif

    return reportProgress(algorithm->read());
}

#if defined(OS_LINUX)
int MergeTreeSource::schedule()
{
    return async_reading_state->getFD();
}
#endif

}
