// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: writer.cc,v 1.5 2002/04/24 05:02:40 jgg Exp $
/* ######################################################################

   Writer 
   
   The file writer classes. These write various types of output, sources,
   packages and contents.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#ifdef __GNUG__
#pragma implementation "writer.h"
#endif

#include "writer.h"
    
#include <apt-pkg/strutl.h>
#include <apt-pkg/error.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/deblistparser.h>

#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>
#include <iostream>
    
#include "cachedb.h"
#include "apt-ftparchive.h"
#include "multicompress.h"
									/*}}}*/

using namespace std;
FTWScanner *FTWScanner::Owner;

// SetTFRewriteData - Helper for setting rewrite lists			/*{{{*/
// ---------------------------------------------------------------------
/* */
inline void SetTFRewriteData(struct TFRewriteData &tfrd,
			     const char *tag,
			     const char *rewrite,
			     const char *newtag = 0)
{
    tfrd.Tag = tag;
    tfrd.Rewrite = rewrite;
    tfrd.NewTag = newtag;
}
									/*}}}*/

// FTWScanner::FTWScanner - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
FTWScanner::FTWScanner()
{
   ErrorPrinted = false;
   NoLinkAct = !_config->FindB("APT::FTPArchive::DeLinkAct",true);
   TmpExt = 0;
   Ext[0] = 0;
   RealPath = 0;
   long PMax = pathconf(".",_PC_PATH_MAX);
   if (PMax > 0)
      RealPath = new char[PMax];
}
									/*}}}*/
// FTWScanner::Scanner - FTW Scanner					/*{{{*/
// ---------------------------------------------------------------------
/* This is the FTW scanner, it processes each directory element in the 
   directory tree. */
int FTWScanner::Scanner(const char *File,const struct stat *sb,int Flag)
{
   if (Flag == FTW_DNR)
   {
      Owner->NewLine(1);
      c1out << "W: Unable to read directory " << File << endl;
   }   
   if (Flag == FTW_NS)
   {
      Owner->NewLine(1);
      c1out << "W: Unable to stat " << File << endl;
   }   
   if (Flag != FTW_F)
      return 0;

   // See if it is a .deb
   if (strlen(File) < 4)
      return 0;
   
   unsigned CurExt = 0;
   for (; Owner->Ext[CurExt] != 0; CurExt++)
      if (strcmp(File+strlen(File)-strlen(Owner->Ext[CurExt]),
		 Owner->Ext[CurExt]) == 0)
	 break;
   if (Owner->Ext[CurExt] == 0)
      return 0;

   /* Process it. If the file is a link then resolve it into an absolute
      name.. This works best if the directory components the scanner are
      given are not links themselves. */
   char Jnk[2];
   Owner->OriginalPath = File;
   if (Owner->RealPath != 0 && readlink(File,Jnk,sizeof(Jnk)) != -1 &&
       realpath(File,Owner->RealPath) != 0)
      Owner->DoPackage(Owner->RealPath);
   else
      Owner->DoPackage(File);
   
   if (_error->empty() == false)
   {
      // Print any errors or warnings found
      string Err;
      bool SeenPath = false;
      while (_error->empty() == false)
      {
	 Owner->NewLine(1);
	 
	 bool Type = _error->PopMessage(Err);
	 if (Type == true)
	    c1out << "E: " << Err << endl;
	 else
	    c1out << "W: " << Err << endl;
	 
	 if (Err.find(File) != string::npos)
	    SeenPath = true;
      }      
      
      if (SeenPath == false)
	 cerr << "E: Errors apply to file '" << File << "'" << endl;
      return 0;
   }
   
   return 0;
}
									/*}}}*/
// FTWScanner::RecursiveScan - Just scan a directory tree		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::RecursiveScan(string Dir)
{
   /* If noprefix is set then jam the scan root in, so we don't generate
      link followed paths out of control */
   if (InternalPrefix.empty() == true)
   {
      if (realpath(Dir.c_str(),RealPath) == 0)
	 return _error->Errno("realpath","Failed to resolve %s",Dir.c_str());
      InternalPrefix = RealPath;      
   }
   
   // Do recursive directory searching
   Owner = this;
   int Res = ftw(Dir.c_str(),Scanner,30);
   
   // Error treewalking?
   if (Res != 0)
   {
      if (_error->PendingError() == false)
	 _error->Errno("ftw","Tree walking failed");
      return false;
   }
   
   return true;
}
									/*}}}*/
// FTWScanner::LoadFileList - Load the file list from a file		/*{{{*/
// ---------------------------------------------------------------------
/* This is an alternative to using FTW to locate files, it reads the list
   of files from another file. */
bool FTWScanner::LoadFileList(string Dir,string File)
{
   /* If noprefix is set then jam the scan root in, so we don't generate
      link followed paths out of control */
   if (InternalPrefix.empty() == true)
   {
      if (realpath(Dir.c_str(),RealPath) == 0)
	 return _error->Errno("realpath","Failed to resolve %s",Dir.c_str());
      InternalPrefix = RealPath;      
   }
   
   Owner = this;
   FILE *List = fopen(File.c_str(),"r");
   if (List == 0)
      return _error->Errno("fopen","Failed to open %s",File.c_str());
   
   /* We are a tad tricky here.. We prefix the buffer with the directory
      name, that way if we need a full path with just use line.. Sneaky and
      fully evil. */
   char Line[1000];
   char *FileStart;
   if (Dir.empty() == true || Dir.end()[-1] != '/')
      FileStart = Line + snprintf(Line,sizeof(Line),"%s/",Dir.c_str());
   else
      FileStart = Line + snprintf(Line,sizeof(Line),"%s",Dir.c_str());   
   while (fgets(FileStart,sizeof(Line) - (FileStart - Line),List) != 0)
   {
      char *FileName = _strstrip(FileStart);
      if (FileName[0] == 0)
	 continue;
	 
      if (FileName[0] != '/')
      {
	 if (FileName != FileStart)
	    memmove(FileStart,FileName,strlen(FileStart));
	 FileName = Line;
      }
      
      struct stat St;
      int Flag = FTW_F;
      if (stat(FileName,&St) != 0)
	 Flag = FTW_NS;

      if (Scanner(FileName,&St,Flag) != 0)
	 break;
   }
  
   fclose(List);
   return true;
}
									/*}}}*/
// FTWScanner::Delink - Delink symlinks					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::Delink(string &FileName,const char *OriginalPath,
			unsigned long &DeLinkBytes,
			struct stat &St)
{
   // See if this isn't an internaly prefix'd file name.
   if (InternalPrefix.empty() == false &&
       InternalPrefix.length() < FileName.length() && 
       stringcmp(FileName.begin(),FileName.begin() + InternalPrefix.length(),
		 InternalPrefix.begin(),InternalPrefix.end()) != 0)
   {
      if (DeLinkLimit != 0 && DeLinkBytes/1024 < DeLinkLimit)
      {
	 // Tidy up the display
	 if (DeLinkBytes == 0)
	    cout << endl;
	 
	 NewLine(1);
	 c1out << " DeLink " << (OriginalPath + InternalPrefix.length())
	    << " [" << SizeToStr(St.st_size) << "B]" << endl << flush;
	 
	 if (NoLinkAct == false)
	 {
	    char OldLink[400];
	    if (readlink(OriginalPath,OldLink,sizeof(OldLink)) == -1)
	       _error->Errno("readlink","Failed to readlink %s",OriginalPath);
	    else
	    {
	       if (unlink(OriginalPath) != 0)
		  _error->Errno("unlink","Failed to unlink %s",OriginalPath);
	       else
	       {
		  if (link(FileName.c_str(),OriginalPath) != 0)
		  {
		     // Panic! Restore the symlink
		     symlink(OldLink,OriginalPath);
		     return _error->Errno("link","*** Failed to link %s to %s",
					  FileName.c_str(),
					  OriginalPath);
		  }	       
	       }
	    }	    
	 }
	 
	 DeLinkBytes += St.st_size;
	 if (DeLinkBytes/1024 >= DeLinkLimit)
	    c1out << " DeLink limit of " << SizeToStr(DeLinkBytes) << "B hit." << endl;      
      }
      
      FileName = OriginalPath;
   }
   
   return true;
}
									/*}}}*/
// FTWScanner::SetExts - Set extensions to support			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::SetExts(string Vals)
{
   delete [] TmpExt;
   TmpExt = new char[Vals.length()+1];
   strcpy(TmpExt,Vals.c_str());
   return TokSplitString(' ',TmpExt,(char **)Ext,sizeof(Ext)/sizeof(Ext[0]));
}
									/*}}}*/

// PackagesWriter::PackagesWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
PackagesWriter::PackagesWriter(string DB,string Overrides,string ExtOverrides) :
		    Db(DB),Stats(Db.Stats)
{
   Output = stdout;
   Ext[0] = ".deb";
   Ext[1] = 0;
   DeLinkLimit = 0;
   
   // Process the command line options
   DoMD5 = _config->FindB("APT::FTPArchive::MD5",true);
   DoContents = _config->FindB("APT::FTPArchive::Contents",true);
   NoOverride = _config->FindB("APT::FTPArchive::NoOverrideMsg",false);

   if (Db.Loaded() == false)
      DoContents = false;
      
   // Read the override file
   if (Overrides.empty() == false && Over.ReadOverride(Overrides) == false)
      return;
   else
      NoOverride = true;

   if (ExtOverrides.empty() == false)
      Over.ReadExtraOverride(ExtOverrides);

   _error->DumpErrors();
}
									/*}}}*/
// PackagesWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* This method takes a package and gets its control information and 
   MD5 then writes out a control record with the proper fields rewritten
   and the path/size/hash appended. */
bool PackagesWriter::DoPackage(string FileName)
{      
   // Open the archive
   FileFd F(FileName,FileFd::ReadOnly);
   if (_error->PendingError() == true)
      return false;
   
   // Stat the file for later
   struct stat St;
   if (fstat(F.Fd(),&St) != 0)
      return _error->Errno("fstat","Failed to stat %s",FileName.c_str());

   // Pull all the data we need form the DB
   string MD5Res;
   if (Db.SetFile(FileName,St,&F) == false ||
       Db.LoadControl() == false ||
       (DoContents == true && Db.LoadContents(true) == false) ||
       (DoMD5 == true && Db.GetMD5(MD5Res,false) == false))
      return false;

   if (Delink(FileName,OriginalPath,Stats.DeLinkBytes,St) == false)
      return false;
   
   // Lookup the overide information
   pkgTagSection &Tags = Db.Control.Section;
   string Package = Tags.FindS("Package");
   Override::Item Tmp;
   Override::Item *OverItem = Over.GetItem(Package);
   
   if (Package.empty() == true)
      return _error->Error("Archive had no package field");
   
   // If we need to do any rewriting of the header do it now..
   if (OverItem == 0)
   {
      if (NoOverride == false)
      {
	 NewLine(1);
	 c1out << "  " << Package << " has no override entry" << endl;
      }
      
      OverItem = &Tmp;
      Tmp.FieldOverride["Section"] = Tags.FindS("Section");
      Tmp.Priority = Tags.FindS("Priority");
   }

   char Size[40];
   sprintf(Size,"%lu",St.st_size);
   
   // Strip the DirStrip prefix from the FileName and add the PathPrefix
   string NewFileName;
   if (DirStrip.empty() == false &&
       FileName.length() > DirStrip.length() &&
       stringcmp(FileName.begin(),FileName.begin() + DirStrip.length(),
		 DirStrip.begin(),DirStrip.end()) == 0)
      NewFileName = string(FileName.begin() + DirStrip.length(),FileName.end());
   else 
      NewFileName = FileName;
   if (PathPrefix.empty() == false)
      NewFileName = flCombine(PathPrefix,NewFileName);
          
   // This lists all the changes to the fields we are going to make.
   // (7 hardcoded + maintainer + suggests + end marker)
   TFRewriteData Changes[6+2+OverItem->FieldOverride.size()+1];

   unsigned int End = 0;
   SetTFRewriteData(Changes[End++], "Size", Size);
   SetTFRewriteData(Changes[End++], "MD5sum", MD5Res.c_str());
   SetTFRewriteData(Changes[End++], "Filename", NewFileName.c_str());
   SetTFRewriteData(Changes[End++], "Priority", OverItem->Priority.c_str());
   SetTFRewriteData(Changes[End++], "Status", 0);
   SetTFRewriteData(Changes[End++], "Optional", 0);

   // Rewrite the maintainer field if necessary
   bool MaintFailed;
   string NewMaint = OverItem->SwapMaint(Tags.FindS("Maintainer"),MaintFailed);
   if (MaintFailed == true)
   {
      if (NoOverride == false)
      {
	 NewLine(1);
	 c1out << "  " << Package << " maintainer is " <<
	       Tags.FindS("Maintainer") << " not " <<
  	       OverItem->OldMaint << endl;
      }      
   }
   
   if (NewMaint.empty() == false)
      SetTFRewriteData(Changes[End++], "Maintainer", NewMaint.c_str());
   
   /* Get rid of the Optional tag. This is an ugly, ugly, ugly hack that
      dpkg-scanpackages does.. Well sort of. dpkg-scanpackages just does renaming
      but dpkg does this append bit. So we do the append bit, at least that way the
      status file and package file will remain similar. There are other transforms
      but optional is the only legacy one still in use for some lazy reason. */
   string OptionalStr = Tags.FindS("Optional");
   if (OptionalStr.empty() == false)
   {
      if (Tags.FindS("Suggests").empty() == false)
	 OptionalStr = Tags.FindS("Suggests") + ", " + OptionalStr;
      SetTFRewriteData(Changes[End++], "Suggests", OptionalStr.c_str());
   }

   for (map<string,string>::iterator I = OverItem->FieldOverride.begin(); 
        I != OverItem->FieldOverride.end(); I++) 
      SetTFRewriteData(Changes[End++],I->first.c_str(),I->second.c_str());

   SetTFRewriteData(Changes[End++], 0, 0);

   // Rewrite and store the fields.
   if (TFRewrite(Output,Tags,TFRewritePackageOrder,Changes) == false)
      return false;
   fprintf(Output,"\n");

   return Db.Finish();
}
									/*}}}*/

// SourcesWriter::SourcesWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
SourcesWriter::SourcesWriter(string BOverrides,string SOverrides,
			     string ExtOverrides)
{
   Output = stdout;
   Ext[0] = ".dsc";
   Ext[1] = 0;
   DeLinkLimit = 0;
   Buffer = 0;
   BufSize = 0;
   
   // Process the command line options
   NoOverride = _config->FindB("APT::FTPArchive::NoOverrideMsg",false);

   // Read the override file
   if (BOverrides.empty() == false && BOver.ReadOverride(BOverrides) == false)
      return;
   else
      NoOverride = true;

   if (ExtOverrides.empty() == false)
      SOver.ReadExtraOverride(ExtOverrides);
   
   if (SOverrides.empty() == false && FileExists(SOverrides) == true)
      SOver.ReadOverride(SOverrides,true);
}
									/*}}}*/
// SourcesWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool SourcesWriter::DoPackage(string FileName)
{      
   // Open the archive
   FileFd F(FileName,FileFd::ReadOnly);
   if (_error->PendingError() == true)
      return false;
   
   // Stat the file for later
   struct stat St;
   if (fstat(F.Fd(),&St) != 0)
      return _error->Errno("fstat","Failed to stat %s",FileName.c_str());

   if (St.st_size > 128*1024)
      return _error->Error("DSC file '%s' is too large!",FileName.c_str());
         
   if (BufSize < (unsigned)St.st_size+1)
   {
      BufSize = St.st_size+1;
      Buffer = (char *)realloc(Buffer,St.st_size+1);
   }
   
   if (F.Read(Buffer,St.st_size) == false)
      return false;

   // Hash the file
   char *Start = Buffer;
   char *BlkEnd = Buffer + St.st_size;
   MD5Summation MD5;
   MD5.Add((unsigned char *)Start,BlkEnd - Start);
      
   // Add an extra \n to the end, just in case
   *BlkEnd++ = '\n';
   
   /* Remove the PGP trailer. Some .dsc's have this without a blank line 
      before */
   const char *Key = "-----BEGIN PGP SIGNATURE-----";
   for (char *MsgEnd = Start; MsgEnd < BlkEnd - strlen(Key) -1; MsgEnd++)
   {
      if (*MsgEnd == '\n' && strncmp(MsgEnd+1,Key,strlen(Key)) == 0)
      {
	 MsgEnd[1] = '\n';
	 break;
      }      
   }
   
   /* Read records until we locate the Source record. This neatly skips the
      GPG header (which is RFC822 formed) without any trouble. */
   pkgTagSection Tags;
   do
   {
      unsigned Pos;
      if (Tags.Scan(Start,BlkEnd - Start) == false)
	 return _error->Error("Could not find a record in the DSC '%s'",FileName.c_str());
      if (Tags.Find("Source",Pos) == true)
	 break;
      Start += Tags.size();
   }
   while (1);
   Tags.Trim();
      
   // Lookup the overide information, finding first the best priority.
   string BestPrio;
   char Buffer[1000];
   string Bins = Tags.FindS("Binary");
   Override::Item *OverItem = 0;
   if (Bins.empty() == false && Bins.length() < sizeof(Buffer))
   {
      strcpy(Buffer,Bins.c_str());
      
      // Ignore too-long errors.
      char *BinList[400];
      TokSplitString(',',Buffer,BinList,sizeof(BinList)/sizeof(BinList[0]));
      
      // Look at all the binaries
      unsigned char BestPrioV = pkgCache::State::Extra;
      for (unsigned I = 0; BinList[I] != 0; I++)
      {
	 Override::Item *Itm = BOver.GetItem(BinList[I]);
	 if (Itm == 0)
	    continue;
	 if (OverItem == 0)
	    OverItem = Itm;

	 unsigned char NewPrioV = debListParser::GetPrio(Itm->Priority);
	 if (NewPrioV < BestPrioV || BestPrio.empty() == true)
	 {
	    BestPrioV = NewPrioV;
	    BestPrio = Itm->Priority;
	 }	 
      }
   }
   
   // If we need to do any rewriting of the header do it now..
   Override::Item Tmp;   
   if (OverItem == 0)
   {
      if (NoOverride == false)
      {
	 NewLine(1);	 
	 c1out << "  " << Tags.FindS("Source") << " has no override entry" << endl;
      }
      
      OverItem = &Tmp;
   }
   
   Override::Item *SOverItem = SOver.GetItem(Tags.FindS("Source"));
   if (SOverItem == 0)
   {
      SOverItem = BOver.GetItem(Tags.FindS("Source"));
      if (SOverItem == 0)
	 SOverItem = OverItem;
   }
   
   // Add the dsc to the files hash list
   char Files[1000];
   snprintf(Files,sizeof(Files),"\n %s %lu %s\n %s",
	    string(MD5.Result()).c_str(),St.st_size,
	    flNotDir(FileName).c_str(),
	    Tags.FindS("Files").c_str());
   
   // Strip the DirStrip prefix from the FileName and add the PathPrefix
   string NewFileName;
   if (DirStrip.empty() == false &&
       FileName.length() > DirStrip.length() &&
       stringcmp(DirStrip,OriginalPath,OriginalPath + DirStrip.length()) == 0)
      NewFileName = string(OriginalPath + DirStrip.length());
   else 
      NewFileName = OriginalPath;
   if (PathPrefix.empty() == false)
      NewFileName = flCombine(PathPrefix,NewFileName);

   string Directory = flNotFile(OriginalPath);
   string Package = Tags.FindS("Source");

   // Perform the delinking operation over all of the files
   string ParseJnk;
   const char *C = Files;
   for (;isspace(*C); C++);
   while (*C != 0)
   {   
      // Parse each of the elements
      if (ParseQuoteWord(C,ParseJnk) == false ||
	  ParseQuoteWord(C,ParseJnk) == false ||
	  ParseQuoteWord(C,ParseJnk) == false)
	 return _error->Error("Error parsing file record");
      
      char Jnk[2];
      string OriginalPath = Directory + ParseJnk;
      if (RealPath != 0 && readlink(OriginalPath.c_str(),Jnk,sizeof(Jnk)) != -1 &&
	  realpath(OriginalPath.c_str(),RealPath) != 0)
      {
	 string RP = RealPath;
	 if (Delink(RP,OriginalPath.c_str(),Stats.DeLinkBytes,St) == false)
	    return false;
      }
   }

   Directory = flNotFile(NewFileName);
   if (Directory.length() > 2)
      Directory.erase(Directory.end()-1);

   // This lists all the changes to the fields we are going to make.
   // (5 hardcoded + maintainer + end marker)
   TFRewriteData Changes[5+1+SOverItem->FieldOverride.size()+1];

   unsigned int End = 0;
   SetTFRewriteData(Changes[End++],"Source",Package.c_str(),"Package");
   SetTFRewriteData(Changes[End++],"Files",Files);
   if (Directory != "./")
      SetTFRewriteData(Changes[End++],"Directory",Directory.c_str());
   SetTFRewriteData(Changes[End++],"Priority",BestPrio.c_str());
   SetTFRewriteData(Changes[End++],"Status",0);

   // Rewrite the maintainer field if necessary
   bool MaintFailed;
   string NewMaint = OverItem->SwapMaint(Tags.FindS("Maintainer"),MaintFailed);
   if (MaintFailed == true)
   {
      if (NoOverride == false)
      {
	 NewLine(1);	 
	 c1out << "  " << Package << " maintainer is " <<
	       Tags.FindS("Maintainer") << " not " <<
  	       OverItem->OldMaint << endl;
      }      
   }
   if (NewMaint.empty() == false)
      SetTFRewriteData(Changes[End++], "Maintainer", NewMaint.c_str());
   
   for (map<string,string>::iterator I = SOverItem->FieldOverride.begin(); 
        I != SOverItem->FieldOverride.end(); I++) 
      SetTFRewriteData(Changes[End++],I->first.c_str(),I->second.c_str());

   SetTFRewriteData(Changes[End++], 0, 0);
      
   // Rewrite and store the fields.
   if (TFRewrite(Output,Tags,TFRewriteSourceOrder,Changes) == false)
      return false;
   fprintf(Output,"\n");

   Stats.Packages++;
   
   return true;
}
									/*}}}*/

// ContentsWriter::ContentsWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
ContentsWriter::ContentsWriter(string DB) : 
		    Db(DB), Stats(Db.Stats)

{
   Ext[0] = ".deb";
   Ext[1] = 0;
   Output = stdout;
}
									/*}}}*/
// ContentsWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* If Package is the empty string the control record will be parsed to
   determine what the package name is. */
bool ContentsWriter::DoPackage(string FileName,string Package)
{
   // Open the archive
   FileFd F(FileName,FileFd::ReadOnly);
   if (_error->PendingError() == true)
      return false;
   
   // Stat the file for later
   struct stat St;
   if (fstat(F.Fd(),&St) != 0)
      return _error->Errno("fstat","Failed too stat %s",FileName.c_str());

   // Ready the DB
   if (Db.SetFile(FileName,St,&F) == false || 
       Db.LoadContents(false) == false)
      return false;

   // Parse the package name
   if (Package.empty() == true)
   {
      if (Db.LoadControl() == false)
	 return false;
      Package = Db.Control.Section.FindS("Package");
   }

   Db.Contents.Add(Gen,Package);
   
   return Db.Finish();
}
									/*}}}*/
// ContentsWriter::ReadFromPkgs - Read from a packages file		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ContentsWriter::ReadFromPkgs(string PkgFile,string PkgCompress)
{
   MultiCompress Pkgs(PkgFile,PkgCompress,0,false);
   if (_error->PendingError() == true)
      return false;
   
   // Open the package file
   int CompFd = -1;
   int Proc = -1;
   if (Pkgs.OpenOld(CompFd,Proc) == false)
      return false;
   
   // No auto-close FD
   FileFd Fd(CompFd,false);   
   pkgTagFile Tags(&Fd);
   if (_error->PendingError() == true)
   {
      Pkgs.CloseOld(CompFd,Proc);
      return false;
   }
   
   // Parse.
   pkgTagSection Section;
   while (Tags.Step(Section) == true)
   {
      string File = flCombine(Prefix,Section.FindS("FileName"));
      string Package = Section.FindS("Section");
      if (Package.empty() == false && Package.end()[-1] != '/')
      {
	 Package += '/';
	 Package += Section.FindS("Package");
      }
      else
	 Package += Section.FindS("Package");
	 
      DoPackage(File,Package);
      if (_error->empty() == false)
      {
	 _error->Error("Errors apply to file '%s'",File.c_str());
	 _error->DumpErrors();
      }
   }
   
   // Tidy the compressor
   if (Pkgs.CloseOld(CompFd,Proc) == false)
      return false;
   
   return true;
}
									/*}}}*/
