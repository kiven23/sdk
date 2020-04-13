/**
 * @file filesystem.cpp
 * @brief Generic host filesystem access interfaces
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#include "mega/filesystem.h"
#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/logging.h"
#include "mega/mega_utf8proc.h"

namespace mega {
FileSystemAccess::FileSystemAccess()
    : waiter(NULL)
    , skip_errorreport(false)
    , transient_error(false)
    , notifyerr(false)
    , notifyfailed(false)
    , target_exists(false)
    , client(NULL)
{
}

void FileSystemAccess::captimestamp(m_time_t* t)
{
    // FIXME: remove upper bound before the year 2100 and upgrade server-side timestamps to BIGINT
    if (*t > (uint32_t)-1) *t = (uint32_t)-1;
    else if (*t < 0) *t = 0;
}

bool FileSystemAccess::islchex(char c) const
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// is c allowed in local fs names?
bool FileSystemAccess::islocalfscompatible(unsigned char c) const
{
    return c >= ' ' && !strchr("\\/:?\"<>|*", c);
}

// replace characters that are not allowed in local fs names with a %xx escape sequence
void FileSystemAccess::escapefsincompatible(string* name) const
{
    if (!name->compare(".."))
    {
        name->replace(0, 2, "%2e%2e");
        return;
    }
    if (!name->compare("."))
    {
        name->replace(0, 1, "%2e");
        return;
    }

    char buf[4];
    unsigned char c;

    // replace all occurrences of a badchar with %xx
    for (size_t i = name->size(); i--; )
    {
        c = (unsigned char)(*name)[i];

        if (!islocalfscompatible(c))
        {
            sprintf(buf, "%%%02x", c);
            name->replace(i, 1, buf);
        }
    }
}

void FileSystemAccess::unescapefsincompatible(string* name) const
{
    if (!name->compare("%2e%2e"))
    {
        name->replace(0, 6, "..");
        return;
    }
    if (!name->compare("%2e"))
    {
        name->replace(0, 3, ".");
        return;
    }
    for (int i = int(name->size()) - 2; i-- > 0; )
    {
        // conditions for unescaping: %xx must be well-formed and encode an incompatible character
        if ((*name)[i] == '%' && islchex((*name)[i + 1]) && islchex((*name)[i + 2]))
        {
            char c = static_cast<char>((MegaClient::hexval((*name)[i + 1]) << 4) + MegaClient::hexval((*name)[i + 2]));

            if (!islocalfscompatible((unsigned char)c))
            {
                name->replace(i, 3, &c, 1);
            }
        }
    }
}

// escape forbidden characters, then convert to local encoding
void FileSystemAccess::name2local(string* filename) const
{
    escapefsincompatible(filename);

    string t = *filename;

    path2local(&t, filename);
}

void FileSystemAccess::normalize(string* filename) const
{
    if (!filename) return;

    const char* cfilename = filename->c_str();
    size_t fnsize = filename->size();
    string result;

    for (size_t i = 0; i < fnsize; )
    {
        // allow NUL bytes between valid UTF-8 sequences
        if (!cfilename[i])
        {
            result.append("", 1);
            i++;
            continue;
        }

        const char* substring = cfilename + i;
        char* normalized = (char*)utf8proc_NFC((uint8_t*)substring);

        if (!normalized)
        {
            filename->clear();
            return;
        }

        result.append(normalized);
        free(normalized);

        i += strlen(substring);
    }

    *filename = result;
}

// convert from local encoding, then unescape escaped forbidden characters
void FileSystemAccess::local2name(string* filename) const
{
    string t = *filename;

    local2path(&t, filename);

    unescapefsincompatible(filename);
}

// default DirNotify: no notification available
DirNotify::DirNotify(const LocalPath& clocalbasepath, const LocalPath& cignore)
{
    localbasepath = clocalbasepath;
    ignore = cignore;

    failed = 1;
    failreason = "Not initialized";
    error = 0;
    sync = NULL;
}

// notify base LocalNode + relative path/filename
void DirNotify::notify(notifyqueue q, LocalNode* l, LocalPath path, bool immediate)
{
#ifdef ENABLE_SYNC
    if ((q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
            && notifyq[q].size()
            && notifyq[q].back().localnode == l
            && notifyq[q].back().path == path)
    {
        if (notifyq[q].back().timestamp)
        {
            notifyq[q].back().timestamp = immediate ? 0 : Waiter::ds;
        }
        LOG_debug << "Repeated notification skipped";
        return;
    }

    if (!immediate && sync && !sync->initializing && q == DirNotify::DIREVENTS)
    {
        LocalPath tmppath;
        if (l)
        {
            tmppath = l->getLocalPath();
        }

        if (!path.empty())
        {
            tmppath.separatorAppend(path, *sync->client->fsaccess, false);
        }
        attr_map::iterator ait;
        auto fa = sync->client->fsaccess->newfileaccess(false);
        bool success = fa->fopen(tmppath, false, false);
        LocalNode *ll = sync->localnodebypath(l, path);
        if ((!ll && !success && !fa->retry) // deleted file
            || (ll && success && ll->node && ll->node->localnode == ll
                && (ll->type != FILENODE || (*(FileFingerprint *)ll) == (*(FileFingerprint *)ll->node))
                && (ait = ll->node->attrs.map.find('n')) != ll->node->attrs.map.end()
                && ait->second == ll->name
                && fa->fsidvalid && fa->fsid == ll->fsid && fa->type == ll->type
                && (ll->type != FILENODE || (ll->mtime == fa->mtime && ll->size == fa->size))))
        {
            LOG_debug << "Self filesystem notification skipped";
            return;
        }
    }

    if (q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
    {
        sync->client->syncactivity = true;
    }
#endif

    notifyq[q].resize(notifyq[q].size() + 1);
    notifyq[q].back().timestamp = immediate ? 0 : Waiter::ds;
    notifyq[q].back().localnode = l;
    notifyq[q].back().path = std::move(path);
}

// default: no fingerprint
fsfp_t DirNotify::fsfingerprint() const
{
    return 0;
}

bool DirNotify::fsstableids() const
{
    return true;
}

DirNotify* FileSystemAccess::newdirnotify(LocalPath& localpath, LocalPath& ignore)
{
    return new DirNotify(localpath, ignore);
}

FileAccess::FileAccess(Waiter *waiter)
{
    this->waiter = waiter;
    this->isAsyncOpened = false;
    this->numAsyncReads = 0;
}

FileAccess::~FileAccess()
{
    // All AsyncIOContext objects must be deleted before
    assert(!numAsyncReads && !isAsyncOpened);
}

// open file for reading
bool FileAccess::fopen(LocalPath& name)
{
    nonblocking_localname.editStringDirect()->resize(1);
    updatelocalname(name);

    return sysstat(&mtime, &size);
}

bool FileAccess::isfolder(LocalPath& name)
{
    fopen(name);
    return (type == FOLDERNODE);
}

// check if size and mtime are unchanged, then open for reading
bool FileAccess::openf()
{
    if (nonblocking_localname.empty())
    {
        // file was not opened in nonblocking mode
        return true;
    }

    m_time_t curr_mtime;
    m_off_t curr_size;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening sync file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    return sysopen();
}

void FileAccess::closef()
{
    if (!nonblocking_localname.empty())
    {
        sysclose();
    }
}

void FileAccess::asyncopfinished(void *param)
{
    Waiter *waiter = (Waiter *)param;
    if (waiter)
    {
        waiter->notify();
    }
}

AsyncIOContext *FileAccess::asyncfopen(LocalPath& f)
{
    nonblocking_localname.editStringDirect()->resize(1);
    updatelocalname(f);

    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_READ;

    context->buffer = (byte *)f.editStringDirect()->data();
    context->len = static_cast<unsigned>(f.editStringDirect()->size());
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->pos = size;
    context->fa = this;

    context->failed = !sysstat(&mtime, &size);
    context->retry = this->retry;
    context->finished = true;
    context->userCallback(context->userData);
    return context;
}

bool FileAccess::asyncopenf()
{
    numAsyncReads++;
    if (nonblocking_localname.empty())
    {
        return true;
    }

    if (isAsyncOpened)
    {
        return true;
    }

    m_time_t curr_mtime = 0;
    m_off_t curr_size = 0;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening async file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    LOG_debug << "Opening async file handle for reading";
    bool result = sysopen(true);
    if (result)
    {
        isAsyncOpened = true;
    }
    else
    {
        LOG_warn << "Error opening async file handle (sysopen)";
    }
    return result;
}

void FileAccess::asyncclosef()
{
    numAsyncReads--;
    if (isAsyncOpened && !numAsyncReads)
    {
        LOG_debug << "Closing async file handle";
        isAsyncOpened = false;
        sysclose();
    }
}

AsyncIOContext *FileAccess::asyncfopen(LocalPath& f, bool read, bool write, m_off_t pos)
{
    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_NONE
            | (read ? AsyncIOContext::ACCESS_READ : 0)
            | (write ? AsyncIOContext::ACCESS_WRITE : 0);

    context->buffer = (byte *)f.editStringDirect()->data();
    context->len = static_cast<unsigned>(f.editStringDirect()->size());
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->pos = pos;
    context->fa = this;

    asyncsysopen(context);
    return context;
}

void FileAccess::asyncsysopen(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfread(string *dst, unsigned len, unsigned pad, m_off_t pos)
{
    LOG_verbose << "Async read start";
    dst->resize(len + pad);

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::READ;
    context->pos = pos;
    context->len = len;
    context->pad = pad;
    context->buffer = (byte *)dst->data();
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    if (!asyncopenf())
    {
        LOG_err << "Error in asyncopenf";
        context->failed = true;
        context->retry = this->retry;
        context->finished = true;
        context->userCallback(context->userData);
        return context;
    }

    asyncsysread(context);
    return context;
}

void FileAccess::asyncsysread(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfwrite(const byte* data, unsigned len, m_off_t pos)
{
    LOG_verbose << "Async write start";

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::WRITE;
    context->pos = pos;
    context->len = len;
    context->buffer = (byte *)data;
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    asyncsyswrite(context);
    return context;
}

void FileAccess::asyncsyswrite(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::newasynccontext()
{
    return new AsyncIOContext();
}

bool FileAccess::fread(string* dst, unsigned len, unsigned pad, m_off_t pos)
{
    if (!openf())
    {
        return false;
    }

    bool r;

    dst->resize(len + pad);

    if ((r = sysread((byte*)dst->data(), len, pos)))
    {
        memset((char*)dst->data() + len, 0, pad);
    }

    closef();

    return r;
}

bool FileAccess::frawread(byte* dst, unsigned len, m_off_t pos, bool caller_opened)
{
    if (!caller_opened && !openf())
    {
        return false;
    }

    bool r = sysread(dst, len, pos);

    if (!caller_opened)
    {
        closef();
    }

    return r;
}

AsyncIOContext::AsyncIOContext()
{
    op = NONE;
    pos = 0;
    len = 0;
    pad = 0;
    buffer = NULL;
    waiter = NULL;
    access = ACCESS_NONE;

    userCallback = NULL;
    userData = NULL;
    finished = false;
    failed = false;
    retry = false;
}

AsyncIOContext::~AsyncIOContext()
{
    finish();

    // AsyncIOContext objects must be deleted before the FileAccess object
    if (op == AsyncIOContext::READ)
    {
        fa->asyncclosef();
    }
}

void AsyncIOContext::finish()
{
    if (!finished)
    {
        while (!finished)
        {
            waiter->init(NEVER);
            waiter->wait();
        }

        // We could have been consumed and external event
        waiter->notify();
    }
}

const std::string* LocalPath::editStringDirect() const
{
    // this function for compatibiltiy while converting to use LocalPath class.  TODO: phase out this function
    return const_cast<std::string*>(&localpath);
}

std::string* LocalPath::editStringDirect()
{
    // this function for compatibiltiy while converting to use LocalPath class.  TODO: phase out this function
    return const_cast<std::string*>(&localpath);
}

bool LocalPath::empty() const
{
    return localpath.empty();
}

size_t LocalPath::lastpartlocal(const FileSystemAccess& fsaccess) const
{
    return fsaccess.lastpartlocal(&localpath);
}

void LocalPath::append(const LocalPath& additionalPath)
{
    localpath.append(additionalPath.localpath);
}

void LocalPath::separatorAppend(const LocalPath& additionalPath, const FileSystemAccess& fsaccess, bool separatorAlways)
{
    if (separatorAlways || localpath.size())
    {
        localpath.append(fsaccess.localseparator);
    }

    localpath.append(additionalPath.localpath);
}

void LocalPath::separatorPrepend(const LocalPath& additionalPath, const FileSystemAccess& fsaccess)
{
    localpath.insert(0, fsaccess.localseparator);
    localpath.insert(0, additionalPath.localpath);
}

void LocalPath::trimTrailingSeparator(const FileSystemAccess& fsaccess)
{
    if (localpath.size() >= fsaccess.localseparator.size()
        && !memcmp(localpath.data() + (int(localpath.size()) & -int(fsaccess.localseparator.size())) - fsaccess.localseparator.size(),
            fsaccess.localseparator.data(),
            fsaccess.localseparator.size()))
    {
        localpath.resize((int(localpath.size()) & -int(fsaccess.localseparator.size())) - fsaccess.localseparator.size());
    }
}

bool LocalPath::findNextSeparator(size_t& separatorBytePos, const FileSystemAccess& fsaccess) const
{
    for (;;)
    {
        separatorBytePos = localpath.find(fsaccess.localseparator, separatorBytePos);
        if (separatorBytePos == string::npos) return false;
        if (separatorBytePos % fsaccess.localseparator.size() == 0) return true;
        separatorBytePos++;
    }
}

bool LocalPath::findPrevSeparator(size_t& separatorBytePos, const FileSystemAccess& fsaccess) const
{
    for (;;)
    {
        separatorBytePos = localpath.rfind(fsaccess.localseparator, separatorBytePos);
        if (separatorBytePos == string::npos) return false;
        if (separatorBytePos % fsaccess.localseparator.size() == 0) return true;
        separatorBytePos--;
    } 
}

size_t LocalPath::getLeafnameByteIndex(const FileSystemAccess& fsaccess) const
{
    // todo: take utf8 two byte characters into account
    size_t p = localpath.size();
    while (p && (p -= fsaccess.localseparator.size()))
    {
        if (!memcmp(localpath.data() + p, fsaccess.localseparator.data(), fsaccess.localseparator.size()))
        {
            p += fsaccess.localseparator.size();
            break;
        }
    }
    return p;
}

bool LocalPath::backEqual(size_t bytePos, const string& compareTo) const
{
    auto n = compareTo.size();
    return bytePos + n == localpath.size() && memcmp(compareTo.data(), localpath.data() + bytePos, n);
}

bool LocalPath::backEqual(size_t bytePos, const LocalPath& compareTo) const
{
    auto n = compareTo.localpath.size();
    return bytePos + n == localpath.size() && memcmp(compareTo.localpath.data(), localpath.data() + bytePos, n);
}

LocalPath LocalPath::subpathFrom(size_t bytePos) const
{
    return LocalPath::fromLocalname(localpath.substr(bytePos));
}

string LocalPath::substrTo(size_t bytePos) const
{
    return localpath.substr(0, bytePos);
}

string LocalPath::toPath(const FileSystemAccess& fsaccess) const
{
    string path;
    fsaccess.local2path(const_cast<string*>(&localpath), &path); // todo: const correctness for local2path etc
    return path;
}

string LocalPath::toName(const FileSystemAccess& fsaccess) const
{
    string name = localpath;
    fsaccess.local2name(&name);
    return name;
}


LocalPath LocalPath::fromPath(const string& path, const FileSystemAccess& fsaccess)
{
    LocalPath p;
    fsaccess.path2local(&path, &p.localpath);
    return p;
}

LocalPath LocalPath::fromName(string path, const FileSystemAccess& fsaccess)
{
    fsaccess.name2local(&path);
    return fromLocalname(path);
}

LocalPath LocalPath::fromLocalname(string path)
{
    LocalPath p;
    p.localpath = std::move(path);
    return p;
}

LocalPath LocalPath::tmpNameLocal(const FileSystemAccess& fsaccess)
{
    LocalPath lp;
    fsaccess.tmpnamelocal(lp);
    return lp;
}

bool LocalPath::isContainingPathOf(const LocalPath& path, const FileSystemAccess& fsaccess)
{
    return path.localpath.size() >= localpath.size() 
        && !memcmp(path.localpath.data(), localpath.data(), localpath.size())
        && (path.localpath.size() == localpath.size() ||
           !memcmp(path.localpath.data() + localpath.size(), fsaccess.localseparator.data(), fsaccess.localseparator.size()));
}

ScopedLengthRestore::ScopedLengthRestore(LocalPath& p)
    : path(p)
    , length(path.getLength())
{
}
ScopedLengthRestore::~ScopedLengthRestore()
{
    path.setLength(length);
};


} // namespace
