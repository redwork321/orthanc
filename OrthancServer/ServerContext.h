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


#pragma once

#include "DicomInstanceToStore.h"
#include "IServerListener.h"
#include "LuaScripting.h"
#include "OrthancHttpHandler.h"
#include "ServerIndex.h"

#include "../Core/Cache/MemoryCache.h"
#include "../Core/Cache/SharedArchive.h"
#include "../Core/DicomParsing/ParsedDicomFile.h"
#include "../Core/FileStorage/IStorageArea.h"
#include "../Core/JobsEngine/JobsEngine.h"
#include "../Core/JobsEngine/SetOfInstancesJob.h"
#include "../Core/MultiThreading/SharedMessageQueue.h"
#include "../Core/RestApi/RestApiOutput.h"
#include "../Plugins/Engine/OrthancPlugins.h"

#include <boost/filesystem.hpp>
#include <boost/thread.hpp>


namespace Orthanc
{
  /**
   * This class is responsible for maintaining the storage area on the
   * filesystem (including compression), as well as the index of the
   * DICOM store. It implements the required locking mechanisms.
   **/
  class ServerContext : private JobsRegistry::IObserver
  {
  private:
    class LuaServerListener : public IServerListener
    {
    private:
      ServerContext& context_;

    public:
      LuaServerListener(ServerContext& context) :
        context_(context)
      {
      }

      virtual void SignalStoredInstance(const std::string& publicId,
                                        DicomInstanceToStore& instance,
                                        const Json::Value& simplifiedTags)
      {
        context_.mainLua_.SignalStoredInstance(publicId, instance, simplifiedTags);
      }
    
      virtual void SignalChange(const ServerIndexChange& change)
      {
        context_.mainLua_.SignalChange(change);
      }

      virtual bool FilterIncomingInstance(const DicomInstanceToStore& instance,
                                          const Json::Value& simplified)
      {
        return context_.filterLua_.FilterIncomingInstance(instance, simplified);
      }
    };
    
    class DicomCacheProvider : public ICachePageProvider
    {
    private:
      ServerContext& context_;

    public:
      DicomCacheProvider(ServerContext& context) : context_(context)
      {
      }
      
      virtual IDynamicObject* Provide(const std::string& id);
    };

    class ServerListener
    {
    private:
      IServerListener *listener_;
      std::string      description_;

    public:
      ServerListener(IServerListener& listener,
                     const std::string& description) :
        listener_(&listener),
        description_(description)
      {
      }

      IServerListener& GetListener()
      {
        return *listener_;
      }

      const std::string& GetDescription()
      {
        return description_;
      }
    };

    typedef std::list<ServerListener>  ServerListeners;


    static void ChangeThread(ServerContext* that,
                             unsigned int sleepDelay);

    static void SaveJobsThread(ServerContext* that,
                               unsigned int sleepDelay);

    void ReadDicomAsJsonInternal(std::string& result,
                                 const std::string& instancePublicId);

    void SetupJobsEngine(bool unitTesting,
                         bool loadJobsFromDatabase);

    void SaveJobsEngine();

    virtual void SignalJobSubmitted(const std::string& jobId);

    virtual void SignalJobSuccess(const std::string& jobId);

    virtual void SignalJobFailure(const std::string& jobId);

    ServerIndex index_;
    IStorageArea& area_;

    bool compressionEnabled_;
    bool storeMD5_;
    
    DicomCacheProvider provider_;
    boost::mutex dicomCacheMutex_;
    MemoryCache dicomCache_;
    JobsEngine jobsEngine_;

    LuaScripting mainLua_;
    LuaScripting filterLua_;
    LuaServerListener  luaListener_;

#if ORTHANC_ENABLE_PLUGINS == 1
    OrthancPlugins* plugins_;
#endif

    ServerListeners listeners_;
    boost::recursive_mutex listenersMutex_;

    bool done_;
    bool haveJobsChanged_;
    SharedMessageQueue  pendingChanges_;
    boost::thread  changeThread_;
    boost::thread  saveJobsThread_;
        
    SharedArchive  queryRetrieveArchive_;
    std::string defaultLocalAet_;
    OrthancHttpHandler  httpHandler_;

  public:
    class DicomCacheLocker : public boost::noncopyable
    {
    private:
      ServerContext& that_;
      ParsedDicomFile *dicom_;
      boost::mutex::scoped_lock lock_;

    public:
      DicomCacheLocker(ServerContext& that,
                       const std::string& instancePublicId);

      ~DicomCacheLocker();

      ParsedDicomFile& GetDicom()
      {
        return *dicom_;
      }
    };

    ServerContext(IDatabaseWrapper& database,
                  IStorageArea& area,
                  bool unitTesting,
                  bool loadJobsFromDatabase);

    ~ServerContext();

    ServerIndex& GetIndex()
    {
      return index_;
    }

    void SetCompressionEnabled(bool enabled);

    bool IsCompressionEnabled() const
    {
      return compressionEnabled_;
    }

    void RemoveFile(const std::string& fileUuid,
                    FileContentType type);

    bool AddAttachment(const std::string& resourceId,
                       FileContentType attachmentType,
                       const void* data,
                       size_t size);

    StoreStatus Store(std::string& resultPublicId,
                      DicomInstanceToStore& dicom);

    void AnswerAttachment(RestApiOutput& output,
                          const std::string& resourceId,
                          FileContentType content);

    void ChangeAttachmentCompression(const std::string& resourceId,
                                     FileContentType attachmentType,
                                     CompressionType compression);

    void ReadDicomAsJson(std::string& result,
                         const std::string& instancePublicId,
                         const std::set<DicomTag>& ignoreTagLength);

    void ReadDicomAsJson(Json::Value& result,
                         const std::string& instancePublicId,
                         const std::set<DicomTag>& ignoreTagLength);

    void ReadDicomAsJson(std::string& result,
                         const std::string& instancePublicId)
    {
      std::set<DicomTag> ignoreTagLength;
      ReadDicomAsJson(result, instancePublicId, ignoreTagLength);
    }

    void ReadDicomAsJson(Json::Value& result,
                         const std::string& instancePublicId)
    {
      std::set<DicomTag> ignoreTagLength;
      ReadDicomAsJson(result, instancePublicId, ignoreTagLength);
    }

    void ReadDicom(std::string& dicom,
                   const std::string& instancePublicId)
    {
      ReadAttachment(dicom, instancePublicId, FileContentType_Dicom, true);
    }
    
    // TODO CACHING MECHANISM AT THIS POINT
    void ReadAttachment(std::string& result,
                        const std::string& instancePublicId,
                        FileContentType content,
                        bool uncompressIfNeeded);
    
    void ReadAttachment(std::string& result,
                        const FileInfo& attachment);

    void SetStoreMD5ForAttachments(bool storeMD5);

    bool IsStoreMD5ForAttachments() const
    {
      return storeMD5_;
    }

    JobsEngine& GetJobsEngine()
    {
      return jobsEngine_;
    }

    bool DeleteResource(Json::Value& target,
                        const std::string& uuid,
                        ResourceType expectedType);

    void SignalChange(const ServerIndexChange& change);

    SharedArchive& GetQueryRetrieveArchive()
    {
      return queryRetrieveArchive_;
    }

    const std::string& GetDefaultLocalApplicationEntityTitle() const
    {
      return defaultLocalAet_;
    }

    LuaScripting& GetLuaScripting()
    {
      return mainLua_;
    }

    OrthancHttpHandler& GetHttpHandler()
    {
      return httpHandler_;
    }

    void Stop();

    void Apply(std::list<std::string>& result,
               const ::Orthanc::LookupResource& lookup,
               size_t since,
               size_t limit);


    /**
     * Management of the plugins
     **/

#if ORTHANC_ENABLE_PLUGINS == 1
    void SetPlugins(OrthancPlugins& plugins);

    void ResetPlugins();

    const OrthancPlugins& GetPlugins() const;

    OrthancPlugins& GetPlugins();
#endif

    bool HasPlugins() const;

    void AddChildInstances(SetOfInstancesJob& job,
                           const std::string& publicId);
  };
}
