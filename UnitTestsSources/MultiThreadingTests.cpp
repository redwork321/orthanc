/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2018 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "PrecompiledHeadersUnitTests.h"
#include "gtest/gtest.h"

#include "../Core/FileStorage/MemoryStorageArea.h"
#include "../Core/JobsEngine/JobsEngine.h"
#include "../Core/Logging.h"
#include "../Core/MultiThreading/SharedMessageQueue.h"
#include "../Core/OrthancException.h"
#include "../Core/SerializationToolbox.h"
#include "../Core/SystemToolbox.h"
#include "../Core/Toolbox.h"
#include "../OrthancServer/DatabaseWrapper.h"
#include "../OrthancServer/ServerContext.h"
#include "../OrthancServer/ServerJobs/LuaJobManager.h"
#include "../OrthancServer/ServerJobs/OrthancJobUnserializer.h"

#include "../Core/JobsEngine/Operations/JobOperationValues.h"
#include "../Core/JobsEngine/Operations/NullOperationValue.h"
#include "../Core/JobsEngine/Operations/StringOperationValue.h"
#include "../OrthancServer/ServerJobs/Operations/DicomInstanceOperationValue.h"

#include "../Core/JobsEngine/Operations/LogJobOperation.h"
#include "../OrthancServer/ServerJobs/Operations/DeleteResourceOperation.h"
#include "../OrthancServer/ServerJobs/Operations/ModifyInstanceOperation.h"
#include "../OrthancServer/ServerJobs/Operations/StorePeerOperation.h"
#include "../OrthancServer/ServerJobs/Operations/StoreScuOperation.h"
#include "../OrthancServer/ServerJobs/Operations/SystemCallOperation.h"

#include "../OrthancServer/ServerJobs/ArchiveJob.h"
#include "../OrthancServer/ServerJobs/DicomModalityStoreJob.h"
#include "../OrthancServer/ServerJobs/OrthancPeerStoreJob.h"
#include "../OrthancServer/ServerJobs/ResourceModificationJob.h"


using namespace Orthanc;

namespace
{
  class DummyJob : public IJob
  {
  private:
    bool         fails_;
    unsigned int count_;
    unsigned int steps_;

  public:
    DummyJob() :
      fails_(false),
      count_(0),
      steps_(4)
    {
    }

    explicit DummyJob(bool fails) :
      fails_(fails),
      count_(0),
      steps_(4)
    {
    }

    virtual void Start()
    {
    }

    virtual void SignalResubmit()
    {
    }
    
    virtual JobStepResult ExecuteStep()
    {
      if (fails_)
      {
        return JobStepResult::Failure(ErrorCode_ParameterOutOfRange);
      }
      else if (count_ == steps_ - 1)
      {
        return JobStepResult::Success();
      }
      else
      {
        count_++;
        return JobStepResult::Continue();
      }
    }

    virtual void ReleaseResources()
    {
    }

    virtual float GetProgress()
    {
      return static_cast<float>(count_) / static_cast<float>(steps_ - 1);
    }

    virtual void GetJobType(std::string& type)
    {
      type = "DummyJob";
    }

    virtual bool Serialize(Json::Value& value)
    {
      value = Json::objectValue;
      value["Type"] = "DummyJob";
      return true;
    }

    virtual void GetPublicContent(Json::Value& value)
    {
      value["hello"] = "world";
    }
  };


  class DummyInstancesJob : public SetOfInstancesJob
  {
  protected:
    virtual bool HandleInstance(const std::string& instance)
    {
      return (instance != "nope");
    }

  public:
    DummyInstancesJob()
    {
    }
    
    DummyInstancesJob(const Json::Value& value) :
      SetOfInstancesJob(value)
    {
    }
    
    virtual void ReleaseResources()
    {
    }

    virtual void GetJobType(std::string& s)
    {
      s = "DummyInstancesJob";
    }
  };


  class DummyUnserializer : public GenericJobUnserializer
  {
  public:
    virtual IJob* UnserializeJob(const Json::Value& value)
    {
      if (SerializationToolbox::ReadString(value, "Type") == "DummyInstancesJob")
      {
        return new DummyInstancesJob(value);
      }
      else if (SerializationToolbox::ReadString(value, "Type") == "DummyJob")
      {
        return new DummyJob;
      }
      else
      {
        return GenericJobUnserializer::UnserializeJob(value);
      }
    }
  };

    
  class DynamicInteger : public IDynamicObject
  {
  private:
    int value_;
    std::set<int>& target_;

  public:
    DynamicInteger(int value, std::set<int>& target) : 
      value_(value), target_(target)
    {
    }

    int GetValue() const
    {
      return value_;
    }
  };
}


TEST(MultiThreading, SharedMessageQueueBasic)
{
  std::set<int> s;

  SharedMessageQueue q;
  ASSERT_TRUE(q.WaitEmpty(0));
  q.Enqueue(new DynamicInteger(10, s));
  ASSERT_FALSE(q.WaitEmpty(1));
  q.Enqueue(new DynamicInteger(20, s));
  q.Enqueue(new DynamicInteger(30, s));
  q.Enqueue(new DynamicInteger(40, s));

  std::auto_ptr<DynamicInteger> i;
  i.reset(dynamic_cast<DynamicInteger*>(q.Dequeue(1))); ASSERT_EQ(10, i->GetValue());
  i.reset(dynamic_cast<DynamicInteger*>(q.Dequeue(1))); ASSERT_EQ(20, i->GetValue());
  i.reset(dynamic_cast<DynamicInteger*>(q.Dequeue(1))); ASSERT_EQ(30, i->GetValue());
  ASSERT_FALSE(q.WaitEmpty(1));
  i.reset(dynamic_cast<DynamicInteger*>(q.Dequeue(1))); ASSERT_EQ(40, i->GetValue());
  ASSERT_TRUE(q.WaitEmpty(0));
  ASSERT_EQ(NULL, q.Dequeue(1));
}


TEST(MultiThreading, SharedMessageQueueClean)
{
  std::set<int> s;

  try
  {
    SharedMessageQueue q;
    q.Enqueue(new DynamicInteger(10, s));
    q.Enqueue(new DynamicInteger(20, s));  
    throw OrthancException(ErrorCode_InternalError);
  }
  catch (OrthancException&)
  {
  }
}




static bool CheckState(JobsRegistry& registry,
                       const std::string& id,
                       JobState state)
{
  JobState s;
  if (registry.GetState(s, id))
  {
    return state == s;
  }
  else
  {
    return false;
  }
}


static bool CheckErrorCode(JobsRegistry& registry,
                           const std::string& id,
                           ErrorCode code)
{
  JobInfo s;
  if (registry.GetJobInfo(s, id))
  {
    return code == s.GetStatus().GetErrorCode();
  }
  else
  {
    return false;
  }
}


TEST(JobsRegistry, Priority)
{
  JobsRegistry registry;

  std::string i1, i2, i3, i4;
  registry.Submit(i1, new DummyJob(), 10);
  registry.Submit(i2, new DummyJob(), 30);
  registry.Submit(i3, new DummyJob(), 20);
  registry.Submit(i4, new DummyJob(), 5);  

  registry.SetMaxCompletedJobs(2);

  std::set<std::string> id;
  registry.ListJobs(id);

  ASSERT_EQ(4u, id.size());
  ASSERT_TRUE(id.find(i1) != id.end());
  ASSERT_TRUE(id.find(i2) != id.end());
  ASSERT_TRUE(id.find(i3) != id.end());
  ASSERT_TRUE(id.find(i4) != id.end());

  ASSERT_TRUE(CheckState(registry, i2, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(30, job.GetPriority());
    ASSERT_EQ(i2, job.GetId());

    ASSERT_TRUE(CheckState(registry, i2, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, i2, JobState_Failure));
  ASSERT_TRUE(CheckState(registry, i3, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(20, job.GetPriority());
    ASSERT_EQ(i3, job.GetId());

    job.MarkSuccess();

    ASSERT_TRUE(CheckState(registry, i3, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, i3, JobState_Success));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(10, job.GetPriority());
    ASSERT_EQ(i1, job.GetId());
  }

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(5, job.GetPriority());
    ASSERT_EQ(i4, job.GetId());
  }

  {
    JobsRegistry::RunningJob job(registry, 1);
    ASSERT_FALSE(job.IsValid());
  }

  JobState s;
  ASSERT_TRUE(registry.GetState(s, i1));
  ASSERT_FALSE(registry.GetState(s, i2));  // Removed because oldest
  ASSERT_FALSE(registry.GetState(s, i3));  // Removed because second oldest
  ASSERT_TRUE(registry.GetState(s, i4));

  registry.SetMaxCompletedJobs(1);  // (*)
  ASSERT_FALSE(registry.GetState(s, i1));  // Just discarded by (*)
  ASSERT_TRUE(registry.GetState(s, i4));
}


TEST(JobsRegistry, Simultaneous)
{
  JobsRegistry registry;

  std::string i1, i2;
  registry.Submit(i1, new DummyJob(), 20);
  registry.Submit(i2, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, i1, JobState_Pending));
  ASSERT_TRUE(CheckState(registry, i2, JobState_Pending));

  {
    JobsRegistry::RunningJob job1(registry, 0);
    JobsRegistry::RunningJob job2(registry, 0);

    ASSERT_TRUE(job1.IsValid());
    ASSERT_TRUE(job2.IsValid());

    job1.MarkFailure();
    job2.MarkSuccess();

    ASSERT_TRUE(CheckState(registry, i1, JobState_Running));
    ASSERT_TRUE(CheckState(registry, i2, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, i1, JobState_Failure));
  ASSERT_TRUE(CheckState(registry, i2, JobState_Success));
}


TEST(JobsRegistry, Resubmit)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    job.MarkFailure();

    ASSERT_TRUE(CheckState(registry, id, JobState_Running));

    registry.Resubmit(id);
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(id, job.GetId());

    job.MarkSuccess();
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Success));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
}


TEST(JobsRegistry, Retry)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    job.MarkRetry(0);

    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Retry));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Retry));
  
  registry.ScheduleRetries();
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    job.MarkSuccess();

    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
}


TEST(JobsRegistry, PausePending)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  registry.Pause(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Pause(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Resume(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));
}


TEST(JobsRegistry, PauseRunning)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());

    registry.Resubmit(id);
    job.MarkPause();
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Resubmit(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Resume(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());

    job.MarkSuccess();
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
}


TEST(JobsRegistry, PauseRetry)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());

    job.MarkRetry(0);
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Retry));

  registry.Pause(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));

  registry.Resume(id);
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());

    job.MarkSuccess();
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
}


TEST(JobsRegistry, Cancel)
{
  JobsRegistry registry;

  std::string id;
  registry.Submit(id, new DummyJob(), 10);

  ASSERT_FALSE(registry.Cancel("nope"));

  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));
            
  ASSERT_TRUE(registry.Cancel(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));
  
  ASSERT_TRUE(registry.Cancel(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));
  
  ASSERT_TRUE(registry.Resubmit(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));
  
  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());

    ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));

    job.MarkSuccess();
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));

  ASSERT_TRUE(registry.Cancel(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Success));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));

  registry.Submit(id, new DummyJob(), 10);

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(id, job.GetId());

    ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));

    job.MarkCanceled();
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));

  ASSERT_TRUE(registry.Resubmit(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));

  ASSERT_TRUE(registry.Pause(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Paused));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));

  ASSERT_TRUE(registry.Cancel(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));

  ASSERT_TRUE(registry.Resubmit(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Pending));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));

  {
    JobsRegistry::RunningJob job(registry, 0);
    ASSERT_TRUE(job.IsValid());
    ASSERT_EQ(id, job.GetId());

    ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));
    ASSERT_TRUE(CheckState(registry, id, JobState_Running));

    job.MarkRetry(500);
  }

  ASSERT_TRUE(CheckState(registry, id, JobState_Retry));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_Success));

  ASSERT_TRUE(registry.Cancel(id));
  ASSERT_TRUE(CheckState(registry, id, JobState_Failure));
  ASSERT_TRUE(CheckErrorCode(registry, id, ErrorCode_CanceledJob));
}



TEST(JobsEngine, SubmitAndWait)
{
  JobsEngine engine;
  engine.SetThreadSleep(10);
  engine.SetWorkersCount(3);
  engine.Start();

  ASSERT_TRUE(engine.GetRegistry().SubmitAndWait(new DummyJob(), rand() % 10));
  ASSERT_FALSE(engine.GetRegistry().SubmitAndWait(new DummyJob(true), rand() % 10));

  engine.Stop();
}


TEST(JobsEngine, DISABLED_SequenceOfOperationsJob)
{
  JobsEngine engine;
  engine.SetThreadSleep(10);
  engine.SetWorkersCount(3);
  engine.Start();

  std::string id;
  SequenceOfOperationsJob* job = NULL;

  {
    std::auto_ptr<SequenceOfOperationsJob> a(new SequenceOfOperationsJob);
    job = a.get();
    engine.GetRegistry().Submit(id, a.release(), 0);
  }

  boost::this_thread::sleep(boost::posix_time::milliseconds(500));

  {
    SequenceOfOperationsJob::Lock lock(*job);
    size_t i = lock.AddOperation(new LogJobOperation);
    size_t j = lock.AddOperation(new LogJobOperation);
    size_t k = lock.AddOperation(new LogJobOperation);

    StringOperationValue a("Hello");
    StringOperationValue b("World");
    lock.AddInput(i, a);
    lock.AddInput(i, b);
    
    lock.Connect(i, j);
    lock.Connect(j, k);
  }

  boost::this_thread::sleep(boost::posix_time::milliseconds(2000));

  engine.Stop();

}


TEST(JobsEngine, DISABLED_Lua)
{
  JobsEngine engine;
  engine.SetThreadSleep(10);
  engine.SetWorkersCount(2);
  engine.Start();

  LuaJobManager lua;
  lua.SetMaxOperationsPerJob(5);
  lua.SetTrailingOperationTimeout(200);

  for (size_t i = 0; i < 30; i++)
  {
    boost::this_thread::sleep(boost::posix_time::milliseconds(150));

    LuaJobManager::Lock lock(lua, engine);
    size_t a = lock.AddLogOperation();
    size_t b = lock.AddLogOperation();
    size_t c = lock.AddSystemCallOperation("echo");
    lock.AddStringInput(a, boost::lexical_cast<std::string>(i));
    lock.AddNullInput(a);
    lock.Connect(a, b);
    lock.Connect(a, c);
  }

  boost::this_thread::sleep(boost::posix_time::milliseconds(2000));

  engine.Stop();
}


static bool CheckSameJson(const Json::Value& a,
                          const Json::Value& b)
{
  std::string s = a.toStyledString();
  std::string t = b.toStyledString();

  if (s == t)
  {
    return true;
  }
  else
  {
    LOG(ERROR) << "Expected serialization: " << s;
    LOG(ERROR) << "Actual serialization: " << t;
    return false;
  }
}


static bool CheckIdempotentSerialization(IJobUnserializer& unserializer,
                                         IJob& job)
{
  Json::Value a = 42;
  
  if (!job.Serialize(a))
  {
    return false;
  }
  else
  {
    std::auto_ptr<IJob> unserialized(unserializer.UnserializeJob(a));
  
    Json::Value b = 43;
    if (unserialized->Serialize(b))
    {
      return CheckSameJson(a, b);
    }
    else
    {
      return false;
    }
  }
}


static bool CheckIdempotentSerialization(IJobUnserializer& unserializer,
                                         IJobOperation& operation)
{
  Json::Value a = 42;
  operation.Serialize(a);
  
  std::auto_ptr<IJobOperation> unserialized(unserializer.UnserializeOperation(a));
  
  Json::Value b = 43;
  unserialized->Serialize(b);

  return CheckSameJson(a, b);
}


static bool CheckIdempotentSerialization(IJobUnserializer& unserializer,
                                         JobOperationValue& value)
{
  Json::Value a = 42;
  value.Serialize(a);
  
  std::auto_ptr<JobOperationValue> unserialized(unserializer.UnserializeValue(a));
  
  Json::Value b = 43;
  unserialized->Serialize(b);

  return CheckSameJson(a, b);
}


TEST(JobsSerialization, BadFileFormat)
{
  GenericJobUnserializer unserializer;

  Json::Value s;

  s = Json::objectValue;
  ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

  s = Json::arrayValue;
  ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

  s = "hello";
  ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

  s = 42;
  ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);
}


TEST(JobsSerialization, JobOperationValues)
{
  Json::Value s;

  {
    JobOperationValues values;
    values.Append(new NullOperationValue);
    values.Append(new StringOperationValue("hello"));
    values.Append(new StringOperationValue("world"));

    s = 42;
    values.Serialize(s);
  }

  {
    GenericJobUnserializer unserializer;
    std::auto_ptr<JobOperationValues> values(JobOperationValues::Unserialize(unserializer, s));
    ASSERT_EQ(3u, values->GetSize());
    ASSERT_EQ(JobOperationValue::Type_Null, values->GetValue(0).GetType());
    ASSERT_EQ(JobOperationValue::Type_String, values->GetValue(1).GetType());
    ASSERT_EQ(JobOperationValue::Type_String, values->GetValue(2).GetType());

    ASSERT_EQ("hello", dynamic_cast<const StringOperationValue&>(values->GetValue(1)).GetContent());
    ASSERT_EQ("world", dynamic_cast<const StringOperationValue&>(values->GetValue(2)).GetContent());
  }
}


TEST(JobsSerialization, GenericValues)
{
  GenericJobUnserializer unserializer;
  Json::Value s;

  {
    NullOperationValue null;

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, null));
    null.Serialize(s);
  }

  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

  std::auto_ptr<JobOperationValue> value;
  value.reset(unserializer.UnserializeValue(s));
  
  ASSERT_EQ(JobOperationValue::Type_Null, value->GetType());

  {
    StringOperationValue str("Hello");

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, str));
    str.Serialize(s);
  }

  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);
  value.reset(unserializer.UnserializeValue(s));

  ASSERT_EQ(JobOperationValue::Type_String, value->GetType());
  ASSERT_EQ("Hello", dynamic_cast<StringOperationValue&>(*value).GetContent());
}


TEST(JobsSerialization, GenericOperations)
{   
  DummyUnserializer unserializer;
  Json::Value s;

  {
    LogJobOperation operation;

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  ASSERT_THROW(unserializer.UnserializeJob(s), OrthancException);
  ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);

  {
    std::auto_ptr<IJobOperation> operation;
    operation.reset(unserializer.UnserializeOperation(s));

    // Make sure that we have indeed unserialized a log operation
    Json::Value dummy;
    ASSERT_THROW(dynamic_cast<DeleteResourceOperation&>(*operation).Serialize(dummy), std::bad_cast);
    dynamic_cast<LogJobOperation&>(*operation).Serialize(dummy);
  }
}


TEST(JobsSerialization, GenericJobs)
{   
  Json::Value s;

  // This tests SetOfInstancesJob
  
  {
    DummyInstancesJob job;
    job.SetDescription("description");
    job.AddInstance("hello");
    job.AddInstance("nope");
    job.AddInstance("world");
    job.SetPermissive(true);
    ASSERT_THROW(job.ExecuteStep(), OrthancException);  // Not started yet
    job.Start();
    job.ExecuteStep();
    job.ExecuteStep();

    {
      DummyUnserializer unserializer;
      ASSERT_TRUE(CheckIdempotentSerialization(unserializer, job));
    }
    
    ASSERT_TRUE(job.Serialize(s));
  }

  {
    DummyUnserializer unserializer;
    ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
    ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

    std::auto_ptr<IJob> job;
    job.reset(unserializer.UnserializeJob(s));

    const DummyInstancesJob& tmp = dynamic_cast<const DummyInstancesJob&>(*job);
    ASSERT_FALSE(tmp.IsStarted());
    ASSERT_TRUE(tmp.IsPermissive());
    ASSERT_EQ("description", tmp.GetDescription());
    ASSERT_EQ(3u, tmp.GetInstancesCount());
    ASSERT_EQ(2u, tmp.GetPosition());
    ASSERT_EQ(1u, tmp.GetFailedInstances().size());
    ASSERT_EQ("hello", tmp.GetInstance(0));
    ASSERT_EQ("nope", tmp.GetInstance(1));
    ASSERT_EQ("world", tmp.GetInstance(2));
    ASSERT_TRUE(tmp.IsFailedInstance("nope"));
  }

  // SequenceOfOperationsJob

  {
    SequenceOfOperationsJob job;
    job.SetDescription("hello");

    {
      SequenceOfOperationsJob::Lock lock(job);
      size_t a = lock.AddOperation(new LogJobOperation);
      size_t b = lock.AddOperation(new LogJobOperation);
      lock.Connect(a, b);

      StringOperationValue s1("hello");
      StringOperationValue s2("world");
      lock.AddInput(a, s1);
      lock.AddInput(a, s2);
      lock.SetDicomAssociationTimeout(200);
      lock.SetTrailingOperationTimeout(300);
    }

    ASSERT_EQ(JobStepCode_Continue, job.ExecuteStep().GetCode());

    {
      GenericJobUnserializer unserializer;
      ASSERT_TRUE(CheckIdempotentSerialization(unserializer, job));
    }
    
    ASSERT_TRUE(job.Serialize(s));
  }

  {
    GenericJobUnserializer unserializer;
    ASSERT_THROW(unserializer.UnserializeValue(s), OrthancException);
    ASSERT_THROW(unserializer.UnserializeOperation(s), OrthancException);

    std::auto_ptr<IJob> job;
    job.reset(unserializer.UnserializeJob(s));

    std::string tmp;
    dynamic_cast<SequenceOfOperationsJob&>(*job).GetDescription(tmp);
    ASSERT_EQ("hello", tmp);
  }  
}


static bool IsSameTagValue(ParsedDicomFile& dicom1,
                           ParsedDicomFile& dicom2,
                           DicomTag tag)
{
  std::string a, b;
  return (dicom1.GetTagValue(a, tag) &&
          dicom2.GetTagValue(b, tag) &&
          (a == b));
}
                       


TEST(JobsSerialization, DicomModification)
{   
  Json::Value s;

  ParsedDicomFile source(true);
  source.Insert(DICOM_TAG_STUDY_DESCRIPTION, "Test 1", false);
  source.Insert(DICOM_TAG_SERIES_DESCRIPTION, "Test 2", false);
  source.Insert(DICOM_TAG_PATIENT_NAME, "Test 3", false);

  std::auto_ptr<ParsedDicomFile> modified(source.Clone(true));

  {
    DicomModification modification;
    modification.SetLevel(ResourceType_Series);
    modification.Clear(DICOM_TAG_STUDY_DESCRIPTION);
    modification.Remove(DICOM_TAG_SERIES_DESCRIPTION);
    modification.Replace(DICOM_TAG_PATIENT_NAME, "Test 4", true);

    modification.Apply(*modified);

    s = 42;
    modification.Serialize(s);
  }

  {
    DicomModification modification(s);
    ASSERT_EQ(ResourceType_Series, modification.GetLevel());
    
    std::auto_ptr<ParsedDicomFile> second(source.Clone(true));
    modification.Apply(*second);

    std::string s;
    ASSERT_TRUE(second->GetTagValue(s, DICOM_TAG_STUDY_DESCRIPTION));
    ASSERT_TRUE(s.empty());
    ASSERT_FALSE(second->GetTagValue(s, DICOM_TAG_SERIES_DESCRIPTION));
    ASSERT_TRUE(second->GetTagValue(s, DICOM_TAG_PATIENT_NAME));
    ASSERT_EQ("Test 4", s);

    ASSERT_TRUE(IsSameTagValue(source, *modified, DICOM_TAG_STUDY_INSTANCE_UID));
    ASSERT_TRUE(IsSameTagValue(source, *second, DICOM_TAG_STUDY_INSTANCE_UID));

    ASSERT_FALSE(IsSameTagValue(source, *second, DICOM_TAG_SERIES_INSTANCE_UID));
    ASSERT_TRUE(IsSameTagValue(*modified, *second, DICOM_TAG_SERIES_INSTANCE_UID));
  }
}


TEST(JobsSerialization, DicomInstanceOrigin)
{   
  Json::Value s;
  std::string t;

  {
    DicomInstanceOrigin origin;

    s = 42;
    origin.Serialize(s);
  }

  {
    DicomInstanceOrigin origin(s);
    ASSERT_EQ(RequestOrigin_Unknown, origin.GetRequestOrigin());
    ASSERT_EQ("", std::string(origin.GetRemoteAetC()));
    ASSERT_FALSE(origin.LookupRemoteIp(t));
    ASSERT_FALSE(origin.LookupRemoteAet(t));
    ASSERT_FALSE(origin.LookupCalledAet(t));
    ASSERT_FALSE(origin.LookupHttpUsername(t));
  }

  {
    DicomInstanceOrigin origin(DicomInstanceOrigin::FromDicomProtocol("host", "aet", "called"));

    s = 42;
    origin.Serialize(s);
  }

  {
    DicomInstanceOrigin origin(s);
    ASSERT_EQ(RequestOrigin_DicomProtocol, origin.GetRequestOrigin());
    ASSERT_EQ("aet", std::string(origin.GetRemoteAetC()));
    ASSERT_TRUE(origin.LookupRemoteIp(t));   ASSERT_EQ("host", t);
    ASSERT_TRUE(origin.LookupRemoteAet(t));  ASSERT_EQ("aet", t);
    ASSERT_TRUE(origin.LookupCalledAet(t));  ASSERT_EQ("called", t);
    ASSERT_FALSE(origin.LookupHttpUsername(t));
  }

  {
    DicomInstanceOrigin origin(DicomInstanceOrigin::FromHttp("host", "username"));

    s = 42;
    origin.Serialize(s);
  }

  {
    DicomInstanceOrigin origin(s);
    ASSERT_EQ(RequestOrigin_RestApi, origin.GetRequestOrigin());
    ASSERT_EQ("", std::string(origin.GetRemoteAetC()));
    ASSERT_TRUE(origin.LookupRemoteIp(t));     ASSERT_EQ("host", t);
    ASSERT_FALSE(origin.LookupRemoteAet(t));
    ASSERT_FALSE(origin.LookupCalledAet(t));
    ASSERT_TRUE(origin.LookupHttpUsername(t)); ASSERT_EQ("username", t);
  }

  {
    DicomInstanceOrigin origin(DicomInstanceOrigin::FromLua());

    s = 42;
    origin.Serialize(s);
  }

  {
    DicomInstanceOrigin origin(s);
    ASSERT_EQ(RequestOrigin_Lua, origin.GetRequestOrigin());
    ASSERT_FALSE(origin.LookupRemoteIp(t));
    ASSERT_FALSE(origin.LookupRemoteAet(t));
    ASSERT_FALSE(origin.LookupCalledAet(t));
    ASSERT_FALSE(origin.LookupHttpUsername(t));
  }

  {
    DicomInstanceOrigin origin(DicomInstanceOrigin::FromPlugins());

    s = 42;
    origin.Serialize(s);
  }

  {
    DicomInstanceOrigin origin(s);
    ASSERT_EQ(RequestOrigin_Plugins, origin.GetRequestOrigin());
    ASSERT_FALSE(origin.LookupRemoteIp(t));
    ASSERT_FALSE(origin.LookupRemoteAet(t));
    ASSERT_FALSE(origin.LookupCalledAet(t));
    ASSERT_FALSE(origin.LookupHttpUsername(t));
  }
}


namespace
{
  class OrthancJobsSerialization : public testing::Test
  {
  private:
    MemoryStorageArea              storage_;
    DatabaseWrapper                db_;   // The SQLite DB is in memory
    std::auto_ptr<ServerContext>   context_;
    TimeoutDicomConnectionManager  manager_;

  public:
    OrthancJobsSerialization()
    {
      db_.Open();
      context_.reset(new ServerContext(db_, storage_, true /* running unit tests */,
                                       false /* don't reload jobs */));
    }

    virtual ~OrthancJobsSerialization()
    {
      context_->Stop();
      context_.reset(NULL);
      db_.Close();
    }

    ServerContext& GetContext() 
    {
      return *context_;
    }

    bool CreateInstance(std::string& id)
    {
      // Create a sample DICOM file
      ParsedDicomFile dicom(true);
      dicom.Replace(DICOM_TAG_PATIENT_NAME, std::string("JODOGNE"),
                    false, DicomReplaceMode_InsertIfAbsent);

      DicomInstanceToStore toStore;
      toStore.SetParsedDicomFile(dicom);

      return (context_->Store(id, toStore) == StoreStatus_Success);
    }
  };
}


TEST_F(OrthancJobsSerialization, Values)
{
  std::string id;
  ASSERT_TRUE(CreateInstance(id));

  Json::Value s;
  OrthancJobUnserializer unserializer(GetContext());
    
  {
    DicomInstanceOperationValue instance(GetContext(), id);

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, instance));
    instance.Serialize(s);
  }

  std::auto_ptr<JobOperationValue> value;
  value.reset(unserializer.UnserializeValue(s));
  ASSERT_EQ(JobOperationValue::Type_DicomInstance, value->GetType());
  ASSERT_EQ(id, dynamic_cast<DicomInstanceOperationValue&>(*value).GetId());

  {
    std::string content;
    dynamic_cast<DicomInstanceOperationValue&>(*value).ReadDicom(content);

    ParsedDicomFile dicom(content);
    ASSERT_TRUE(dicom.GetTagValue(content, DICOM_TAG_PATIENT_NAME));
    ASSERT_EQ("JODOGNE", content);
  }
}


TEST_F(OrthancJobsSerialization, Operations)
{
  std::string id;
  ASSERT_TRUE(CreateInstance(id));

  Json::Value s;
  OrthancJobUnserializer unserializer(GetContext()); 

  // DeleteResourceOperation
  
  {
    DeleteResourceOperation operation(GetContext());

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  std::auto_ptr<IJobOperation> operation;

  {
    operation.reset(unserializer.UnserializeOperation(s));

    Json::Value dummy;
    ASSERT_THROW(dynamic_cast<LogJobOperation&>(*operation).Serialize(dummy), std::bad_cast);
    dynamic_cast<DeleteResourceOperation&>(*operation).Serialize(dummy);
  }

  // StorePeerOperation

  {
    WebServiceParameters peer;
    peer.SetUrl("http://localhost/");
    peer.SetUsername("username");
    peer.SetPassword("password");
    peer.SetPkcs11Enabled(true);

    StorePeerOperation operation(peer);

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  {
    operation.reset(unserializer.UnserializeOperation(s));

    const StorePeerOperation& tmp = dynamic_cast<StorePeerOperation&>(*operation);
    ASSERT_EQ("http://localhost/", tmp.GetPeer().GetUrl());
    ASSERT_EQ("username", tmp.GetPeer().GetUsername());
    ASSERT_EQ("password", tmp.GetPeer().GetPassword());
    ASSERT_TRUE(tmp.GetPeer().IsPkcs11Enabled());
  }

  // StoreScuOperation

  {
    RemoteModalityParameters modality;
    modality.SetApplicationEntityTitle("REMOTE");
    modality.SetHost("192.168.1.1");
    modality.SetPortNumber(1000);
    modality.SetManufacturer(ModalityManufacturer_StoreScp);

    StoreScuOperation operation("TEST", modality);

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  {
    operation.reset(unserializer.UnserializeOperation(s));

    const StoreScuOperation& tmp = dynamic_cast<StoreScuOperation&>(*operation);
    ASSERT_EQ("REMOTE", tmp.GetRemoteModality().GetApplicationEntityTitle());
    ASSERT_EQ("192.168.1.1", tmp.GetRemoteModality().GetHost());
    ASSERT_EQ(1000, tmp.GetRemoteModality().GetPortNumber());
    ASSERT_EQ(ModalityManufacturer_StoreScp, tmp.GetRemoteModality().GetManufacturer());
    ASSERT_EQ("TEST", tmp.GetLocalAet());
  }

  // SystemCallOperation

  {
    SystemCallOperation operation(std::string("echo"));
    operation.AddPreArgument("a");
    operation.AddPreArgument("b");
    operation.AddPostArgument("c");

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  {
    operation.reset(unserializer.UnserializeOperation(s));

    const SystemCallOperation& tmp = dynamic_cast<SystemCallOperation&>(*operation);
    ASSERT_EQ("echo", tmp.GetCommand());
    ASSERT_EQ(2u, tmp.GetPreArgumentsCount());
    ASSERT_EQ(1u, tmp.GetPostArgumentsCount());
    ASSERT_EQ("a", tmp.GetPreArgument(0));
    ASSERT_EQ("b", tmp.GetPreArgument(1));
    ASSERT_EQ("c", tmp.GetPostArgument(0));
  }

  // ModifyInstanceOperation

  {
    std::auto_ptr<DicomModification> modification(new DicomModification);
    modification->SetupAnonymization(DicomVersion_2008);
    
    ModifyInstanceOperation operation(GetContext(), RequestOrigin_Lua, modification.release());

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, operation));
    operation.Serialize(s);
  }

  {
    operation.reset(unserializer.UnserializeOperation(s));

    const ModifyInstanceOperation& tmp = dynamic_cast<ModifyInstanceOperation&>(*operation);
    ASSERT_EQ(RequestOrigin_Lua, tmp.GetRequestOrigin());
    ASSERT_TRUE(tmp.GetModification().IsRemoved(DICOM_TAG_STUDY_DESCRIPTION));
  }
}


TEST_F(OrthancJobsSerialization, Jobs)
{
  // ArchiveJob

  Json::Value s;

  {
    boost::shared_ptr<TemporaryFile> tmp(new TemporaryFile);
    ArchiveJob job(tmp, GetContext(), false, false);
    ASSERT_FALSE(job.Serialize(s));  // Cannot serialize this
  }

  // DicomModalityStoreJob

  OrthancJobUnserializer unserializer(GetContext()); 

  {
    RemoteModalityParameters modality;
    modality.SetApplicationEntityTitle("REMOTE");
    modality.SetHost("192.168.1.1");
    modality.SetPortNumber(1000);
    modality.SetManufacturer(ModalityManufacturer_StoreScp);

    DicomModalityStoreJob job(GetContext());
    job.SetLocalAet("LOCAL");
    job.SetRemoteModality(modality);
    job.SetMoveOriginator("MOVESCU", 42);

    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, job));
    ASSERT_TRUE(job.Serialize(s));
  }

  {
    std::auto_ptr<IJob> job;
    job.reset(unserializer.UnserializeJob(s));

    DicomModalityStoreJob& tmp = dynamic_cast<DicomModalityStoreJob&>(*job);
    ASSERT_EQ("LOCAL", tmp.GetLocalAet());
    ASSERT_EQ("REMOTE", tmp.GetRemoteModality().GetApplicationEntityTitle());
    ASSERT_EQ("192.168.1.1", tmp.GetRemoteModality().GetHost());
    ASSERT_EQ(1000, tmp.GetRemoteModality().GetPortNumber());
    ASSERT_EQ(ModalityManufacturer_StoreScp, tmp.GetRemoteModality().GetManufacturer());
    ASSERT_TRUE(tmp.HasMoveOriginator());
    ASSERT_EQ("MOVESCU", tmp.GetMoveOriginatorAet());
    ASSERT_EQ(42, tmp.GetMoveOriginatorId());
  }

  // OrthancPeerStoreJob

  {
    WebServiceParameters peer;
    peer.SetUrl("http://localhost/");
    peer.SetUsername("username");
    peer.SetPassword("password");
    peer.SetPkcs11Enabled(true);

    OrthancPeerStoreJob job(GetContext());
    job.SetPeer(peer);
    
    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, job));
    ASSERT_TRUE(job.Serialize(s));
  }

  {
    std::auto_ptr<IJob> job;
    job.reset(unserializer.UnserializeJob(s));

    OrthancPeerStoreJob& tmp = dynamic_cast<OrthancPeerStoreJob&>(*job);
    ASSERT_EQ("http://localhost/", tmp.GetPeer().GetUrl());
    ASSERT_EQ("username", tmp.GetPeer().GetUsername());
    ASSERT_EQ("password", tmp.GetPeer().GetPassword());
    ASSERT_TRUE(tmp.GetPeer().IsPkcs11Enabled());
  }

  // ResourceModificationJob

  {
    std::auto_ptr<DicomModification> modification(new DicomModification);
    modification->SetupAnonymization(DicomVersion_2008);    

    ResourceModificationJob job(GetContext());
    job.SetModification(modification.release(), true);
    job.SetOrigin(DicomInstanceOrigin::FromLua());
    
    ASSERT_TRUE(CheckIdempotentSerialization(unserializer, job));
    ASSERT_TRUE(job.Serialize(s));
  }

  {
    std::auto_ptr<IJob> job;
    job.reset(unserializer.UnserializeJob(s));

    ResourceModificationJob& tmp = dynamic_cast<ResourceModificationJob&>(*job);
    ASSERT_TRUE(tmp.IsAnonymization());
    ASSERT_EQ(RequestOrigin_Lua, tmp.GetOrigin().GetRequestOrigin());
    ASSERT_TRUE(tmp.GetModification().IsRemoved(DICOM_TAG_STUDY_DESCRIPTION));
  }
}


TEST(JobsSerialization, Registry)
{   
  Json::Value s;
  std::string i1, i2;

  {
    JobsRegistry registry;
    registry.Submit(i1, new DummyJob(), 10);
    registry.Submit(i2, new SequenceOfOperationsJob(), 30);
    registry.Serialize(s);
  }

  {
    DummyUnserializer unserializer;
    JobsRegistry registry(unserializer, s);

    Json::Value t;
    registry.Serialize(t);
    ASSERT_TRUE(CheckSameJson(s, t));
  }
}
