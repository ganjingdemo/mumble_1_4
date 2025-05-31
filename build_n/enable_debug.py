def main():
    f=open("build.ninja","r")
    content = f.read()
    f.close()

    content2 = content.replace("/Ob2 /DNDEBUG","/Ob2 /Zi /DNDEBUG")

    content3 = content2.replace("/machine:x64 /INCREMENTAL:NO", "/machine:x64 /debug /INCREMENTAL:NO")
    
    if content3 == content:
        print("Nothing to replace")
        return
      
    print("Will create backup file")
    f=open("build.ninja.bak","w")
    f.write(content)
    f.close()
    
    
    print("Will update file")
    f=open("build.ninja","w")
    f.write(content3)
    f.close()

main()