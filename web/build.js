const fs=require('fs');
let t=fs.readFileSync('template.html','utf8');
const engine=fs.readFileSync('engine.js','utf8');
if(engine.includes('</script')) throw new Error('engine has </script');
t=t.replace('/*__ENGINE__*/',()=>engine).replace('/*__EXAMPLES__*/',()=>fs.readFileSync('examples.js','utf8'));
fs.writeFileSync('../docs/index.html',t);
console.log('built',t.length);
